/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <bit>
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <unordered_set>
#include <optional>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include <SPIRV/GLSL.std.450.h>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/shader/spirv_builder.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/util/native_surface_copy.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/deferred_command_buffer.h>
#include <rex/graphics/vulkan/render_target_cache.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/ui/graphics_util.h>
#include <rex/ui/vulkan/util.h>

// Experiment: skip the EDRAM ownership-transfer draws between host render
// targets. This title issues about 18 per frame and relies on them - skipping
// them corrupts the character shadow blobs - so it stays off.
REXCVAR_DEFINE_INT32(rt_debug_log_start_frame, 0, "GPU/Vulkan",
                     "First frame of the rt_debug_log_frames window (diagnostic)");

REXCVAR_DEFINE_INT32(rt_debug_log_frames, 0, "GPU/Vulkan",
                     "Log every EDRAM ownership transfer and resolve for this many frames "
                     "(diagnostic)");

REXCVAR_DEFINE_BOOL(native_rt_skip_transfers, false, "GPU/Vulkan",
                    "Skip EDRAM ownership transfer draws on the host render "
                    "target path (breaks character shadows in this title)");

REXCVAR_DEFINE_BOOL(native_rt_image_copies, false, "GPU/Vulkan",
                    "Preserve matching single-sampled surfaces with native image copies on rebind")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(vulkan_2_10_10_10_exact, true, "GPU/Vulkan",
                    "Host render targets: store 2_10_10_10 color in the exact 10:10:10:2 format "
                    "instead of 8 bits per channel, keeping the guest's 10-bit color and 2-bit "
                    "alpha (the 8-bit storage bleached this game's character colors)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_rt_msaa_as_single_sample, true, "GPU/Vulkan",
                    "Native renderer: keep 2x / 4x MSAA surfaces in single-sampled render "
                    "targets with every sample as a pixel, shared with the single-sampled view "
                    "of the same EDRAM, instead of transferring between them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_rt_skip_overwritten_transfers, true, "GPU/Vulkan",
                    "Native renderer: skip EDRAM ownership transfers into the parts of render "
                    "targets that the current draw (a clear) overwrites entirely");

REXCVAR_DEFINE_BOOL(native_rt_cpu_vs_overwrite_proofs, true, "GPU/Vulkan",
                    "Native renderer: also recognize draws that overwrite render targets "
                    "entirely when their vertex shader isn't the XDK clear one (post-processing "
                    "passes, for instance), by running it on the CPU");

REXCVAR_DEFINE_BOOL(native_resolve_copy_free, false, "GPU/Vulkan",
                    "Native renderer: when a resolve copies a whole render target into a "
                    "texture and the next draw overwrites that render target entirely, let the "
                    "texture take over the render target's image instead of copying it "
                    "(resolution-scaled 8_8_8_8 and 2_10_10_10 resolves)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_resolve_copy_free_debug_writeback, false, "GPU/Vulkan",
                    "Native renderer debugging: after a texture takes over a render target's "
                    "image, write the resolved memory from it and reload the texture from that "
                    "memory (checks the write-back of images taken over)");

REXCVAR_DEFINE_BOOL(native_rt_clear_draws_as_clears, true, "GPU/Vulkan",
                    "Native renderer: do the XDK's clear draws that replace everything they "
                    "write with constant values as clears of the attachments, which the GPU can "
                    "do without drawing every pixel (fast clears)");

REXCVAR_DEFINE_BOOL(native_rt_debug_poison_overwrites, false, "GPU/Vulkan",
                    "Native renderer debugging: before each draw proven to overwrite parts of "
                    "render targets entirely, fill those parts with garbage. The final image "
                    "must not change if the proofs are right");

REXCVAR_DEFINE_BOOL(native_rt_defer_overwritten_transfers, true, "GPU/Vulkan",
                    "Native renderer: with native_rt_skip_overwritten_transfers, hold the rest "
                    "of such transfers back while the following draws keep clearing the same "
                    "render targets, and skip what they clear too");

REXCVAR_DEFINE_BOOL(native_resolve, true, "GPU/Vulkan",
                    "Native renderer: draw resolved render targets directly into the textures "
                    "that sample them instead of reloading those textures from guest memory")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_resolve_image_copies, false, "GPU/Vulkan",
                    "Copy matching color resolves into separate native texture images")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_resolve_single_pass, true, "GPU/Vulkan",
                    "Native resolves also write the resolved guest memory in the same pass, "
                    "instead of dumping the render target to the EDRAM buffer and resolving it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_resolve_buffers, false, "GPU/Vulkan",
                    "Native renderer: write resolves into compact GPU buffers, then "
                    "copy the results to the guest memory mirror for existing consumers")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_resolve_buffer_reads, false, "GPU/Vulkan",
                    "Load textures directly from valid compact native resolve buffers")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_resolve_buffer_lazy_memory, false, "GPU/Vulkan",
                    "With native resolve buffer reads, copy outputs to the compatibility "
                    "memory buffers only when another consumer needs them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_resolve_buffer_reuse, false, "GPU/Vulkan",
                    "Reuse matching native GPU resolve buffers with ordered GPU dependencies")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_resolve_buffer_texture_first, false, "GPU/Vulkan",
                    "Keep scaled resolves in native textures when no packed buffer is needed")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(native_resolve_buffer_mb, 128, "GPU/Vulkan",
                     "Maximum MiB of retained native resolve buffers")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_resolve_debug_skip_scaled_memory, false, "GPU/Vulkan",
                    "Measurement only, unsafe: with draw resolution scaling, native resolves keep "
                    "the textures up to date but don't write the scaled resolve memory at all "
                    "(anything later reading that memory gets stale data)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_resolve_scaled_lazy_memory, true, "GPU/Vulkan",
                    "With draw resolution scaling, color native resolves write the scaled resolve "
                    "memory only when something is about to read it (from the texture they "
                    "wrote), instead of in every resolve")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_resolve_scaled_lazy_depth, true, "GPU/Vulkan",
                    "With native_resolve_scaled_lazy_memory, depth native resolves also write the "
                    "scaled resolve memory only when something is about to read it (their "
                    "stencil is kept in a separate capture until then)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_rt_size_by_use, true, "GPU/Vulkan",
                    "Host render targets cover only the rows of EDRAM tiles drawn to (growing "
                    "when more are needed) instead of the whole EDRAM addressing period from "
                    "their base, so they're the size of the surfaces the game renders")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(native_rt_debug_start_tile_rows, 0, "GPU/Vulkan",
                     "Testing native_rt_size_by_use: create render targets with at most this "
                     "many rows of tiles, so they have to grow (0 = off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(native_rt_debug_regrow_interval, 0, "GPU/Vulkan",
                     "Testing native_rt_size_by_use: replace a render target's image with a copy "
                     "every this many times rows are requested for it, even if it has enough "
                     "(0 = off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_resolve_unorm_views, true, "GPU/Vulkan",
                    "Native resolves write 8_8_8_8 and 2_10_10_10 textures (from render targets "
                    "of the same format) and depth textures through a view of their own format, "
                    "which is exact for them, instead of a raw 32-bit integer view, so the "
                    "textures can stay compressed on GPUs that compress color images (AMD DCC), "
                    "saving memory bandwidth")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_resolve_memory_only, true, "GPU/Vulkan",
                    "Native resolves whose destination no texture samples yet write the resolved "
                    "memory directly from the render target, instead of dumping it to the EDRAM "
                    "buffer and resolving from there");

REXCVAR_DEFINE_BOOL(native_resolve_debug_memory_only_all, false, "GPU/Vulkan",
                    "Debug: perform every native resolve as a memory-only one, so the textures "
                    "sampling the destinations reload from the memory it writes (verifies it)");

REXCVAR_DEFINE_BOOL(native_resolve_uniform_stencil, true, "GPU/Vulkan",
                    "Native resolves with draw resolution scaling: track where depth render "
                    "targets have one stencil value (cleared by a draw, not written since), and "
                    "don't capture the stencil of depth resolves inside such an area");

REXCVAR_DEFINE_BOOL(native_resolve_debug_verify_stencil_capture, false, "GPU/Vulkan",
                    "Verification only: lazily written depth resolves capture the stencil both in "
                    "the resolve and by a copy, and write-backs store zero depth wherever the two "
                    "differ (visible in textures reloaded from the memory with "
                    "native_resolve_debug_reload)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_resolve_scaled_single_pass, true, "GPU/Vulkan",
                    "With draw resolution scaling, native resolves also write the scaled resolve "
                    "buffer in the same pass, instead of the render target being dumped to the "
                    "EDRAM buffer and resolved to it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(native_resolve_mask, 3, "GPU/Vulkan",
                     "Native resolve sources to write directly: 1 = depth, 2 = color, 3 = both "
                     "(diagnostic)");

REXCVAR_DEFINE_INT32(native_resolve_only, -1, "GPU/Vulkan",
                     "Write only the resolve with this index within each frame directly "
                     "(diagnostic, -1 = all)");

REXCVAR_DEFINE_BOOL(native_resolve_debug_reload, false, "GPU/Vulkan",
                    "Write resolved textures directly but still reload them from memory "
                    "(diagnostic)");

REXCVAR_DEFINE_INT32(native_resolve_ab_frames, 0, "GPU/Vulkan",
                     "Alternate native resolves on and off every this many frames, logging each "
                     "switch, to compare both in the same scene (diagnostic, 0 = off)");

REXCVAR_DECLARE(bool, vulkan_dynamic_rendering);

// "native" renders into real GPU render targets with fixed-function blending
// and depth/stencil, the way a PC game does. "fsi" emulates the Xenos EDRAM in
// the pixel shader (fragment shader interlock), which is accurate but costs a
// lot of GPU time per pixel. The host path's old character-color defect did
// not reproduce on character-bearing frame traces, and menus, FMV, cutscenes
// and gameplay render identically on both, so "native" is the default.
REXCVAR_DEFINE_STRING(render_target_path_vulkan, "native", "GPU/Vulkan",
                      "Render target path: native (GPU render targets) or fsi (EDRAM "
                      "emulation in the pixel shader)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// DEFINE_string(
//     render_target_path_vulkan, "",
//     "Render target emulation path to use on Vulkan.\n"
//     "Use: [any, fbo, fsi]\n"
//     " fbo:\n"
//     "  Host framebuffers and fixed-function blending and depth / stencil "
//     "testing, copying between render targets when needed.\n"
//     "  Lower accuracy (limited pixel format support).\n"
//     "  Performance limited primarily by render target layout changes requiring "
//     "copying, but generally higher.\n"
//     " fsi:\n"
//     "  Manual pixel packing, blending and depth / stencil testing, with free "
//     "render target layout changes.\n"
//     "  Requires a GPU supporting fragment shader interlock.\n"
//     "  Highest accuracy (all pixel formats handled in software).\n"
//     "  Performance limited primarily by overdraw.\n"
//     " Any other value:\n"
//     "  Choose what is considered the most optimal for the system (currently "
//     "always FB because the FSI path is much slower now).",
//     "GPU");

namespace rex::graphics::vulkan {

// Resolve clears of depth with a non-zero stencil value, for the command
// processor's gpu_wait_stats log.
std::atomic<uint64_t> g_stencil_nonzero_clears{0};


namespace {
// Frames left to log transfers/resolves for; -1 until the first frame reads the cvar.
int32_t g_rt_debug_frames_left = -1;
uint32_t g_rt_debug_frame = 0;
uint32_t g_rt_debug_start_frame = 0;
bool g_rt_debug_active = false;
const char* RtFormatName(bool is_depth, uint32_t format) {
  if (is_depth) {
    return format ? "D24FS8" : "D24S8";
  }
  static const char* kColor[16] = {"8888", "8888_GAMMA", "2_10_10_10", "2_10_10_10_FLOAT",
                                   "16_16", "16_16_16_16", "16_16_FLOAT", "16_16_16_16_FLOAT",
                                   "fmt8", "fmt9", "2_10_10_10_AS_10_10_10_10", "fmt11",
                                   "2_10_10_10_FLOAT_AS_16_16_16_16", "fmt13", "32_FLOAT",
                                   "32_32_FLOAT"};
  return kColor[format & 15];
}
}  // namespace

bool RtDebugLogActive() { return g_rt_debug_active; }

void RtDebugLogBeginFrame() {
  if (g_rt_debug_frames_left < 0) {
    g_rt_debug_start_frame = uint32_t(std::max(0, int32_t(REXCVAR_GET(rt_debug_log_start_frame))));
    g_rt_debug_frames_left = REXCVAR_GET(rt_debug_log_frames);
    // Offline tools (trace_dump) do not run the cvar command line/environment
    // parsing, so accept the environment variable directly as well.
    if (const char* env = std::getenv("REX_RT_DEBUG_LOG_FRAMES")) {
      g_rt_debug_frames_left = std::max(g_rt_debug_frames_left, std::atoi(env));
    }
    if (g_rt_debug_frames_left > 0) {
      REXGPU_WARN("[rt-debug] logging transfers and resolves for {} frames",
                  g_rt_debug_frames_left);
    }
  } else if (g_rt_debug_active && g_rt_debug_frames_left > 0) {
    --g_rt_debug_frames_left;
  }
  g_rt_debug_active = g_rt_debug_frames_left > 0 && g_rt_debug_frame >= g_rt_debug_start_frame;
  if (g_rt_debug_active) {
    REXGPU_INFO("[rt-debug] ---- frame {} ----", g_rt_debug_frame);
  }
  ++g_rt_debug_frame;
}

// EDRAM-emulation cost probe. These transfer draws and resolves exist only
// because the Xenos resolved through dedicated EDRAM that this GPU does not
// have; they are the GPU-side work a native render-target path would delete.
std::atomic<uint32_t> g_edram_transfer_draws{0};
std::atomic<uint32_t> g_edram_resolves{0};


// Generated with `xb buildshaders`.
namespace shaders {
#include "../shaders/vulkan_spirv/host_depth_store_1xmsaa_cs.h"
#include "../shaders/vulkan_spirv/host_depth_store_2xmsaa_cs.h"
#include "../shaders/vulkan_spirv/host_depth_store_4xmsaa_cs.h"
#include "../shaders/vulkan_spirv/passthrough_position_xy_vs.h"
#include "../shaders/vulkan_spirv/resolve_clear_32bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_clear_32bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_clear_64bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_clear_64bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_32bpp_4xmsaa_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_64bpp_4xmsaa_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_128bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_128bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_16bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_16bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_32bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_32bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_64bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_64bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_8bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_8bpp_scaled_cs.h"
}  // namespace shaders

const VulkanRenderTargetCache::ResolveCopyShaderCode
    VulkanRenderTargetCache::kResolveCopyShaders[size_t(
        draw_util::ResolveCopyShaderIndex::kCount)] = {
        {shaders::resolve_fast_32bpp_1x2xmsaa_cs, sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_cs),
         shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_32bpp_4xmsaa_cs, sizeof(shaders::resolve_fast_32bpp_4xmsaa_cs),
         shaders::resolve_fast_32bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_1x2xmsaa_cs, sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_cs),
         shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_4xmsaa_cs, sizeof(shaders::resolve_fast_64bpp_4xmsaa_cs),
         shaders::resolve_fast_64bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_full_8bpp_cs, sizeof(shaders::resolve_full_8bpp_cs),
         shaders::resolve_full_8bpp_scaled_cs, sizeof(shaders::resolve_full_8bpp_scaled_cs)},
        {shaders::resolve_full_16bpp_cs, sizeof(shaders::resolve_full_16bpp_cs),
         shaders::resolve_full_16bpp_scaled_cs, sizeof(shaders::resolve_full_16bpp_scaled_cs)},
        {shaders::resolve_full_32bpp_cs, sizeof(shaders::resolve_full_32bpp_cs),
         shaders::resolve_full_32bpp_scaled_cs, sizeof(shaders::resolve_full_32bpp_scaled_cs)},
        {shaders::resolve_full_64bpp_cs, sizeof(shaders::resolve_full_64bpp_cs),
         shaders::resolve_full_64bpp_scaled_cs, sizeof(shaders::resolve_full_64bpp_scaled_cs)},
        {shaders::resolve_full_128bpp_cs, sizeof(shaders::resolve_full_128bpp_cs),
         shaders::resolve_full_128bpp_scaled_cs, sizeof(shaders::resolve_full_128bpp_scaled_cs)},
};

const VulkanRenderTargetCache::TransferPipelineLayoutInfo
    VulkanRenderTargetCache::kTransferPipelineLayoutInfos[size_t(
        TransferPipelineLayoutIndex::kCount)] = {
        // kColor
        {kTransferUsedDescriptorSetColorTextureBit, kTransferUsedPushConstantDwordAddressBit},
        // kDepth
        {kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordAddressBit},
        // kColorToStencilBit
        {kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordAddressBit | kTransferUsedPushConstantDwordStencilMaskBit},
        // kDepthToStencilBit
        {kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordAddressBit | kTransferUsedPushConstantDwordStencilMaskBit},
        // kColorAndHostDepthTexture
        {kTransferUsedDescriptorSetHostDepthStencilTexturesBit |
             kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kColorAndHostDepthBuffer
        {kTransferUsedDescriptorSetHostDepthBufferBit | kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kDepthAndHostDepthTexture
        {kTransferUsedDescriptorSetHostDepthStencilTexturesBit |
             kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kDepthAndHostDepthBuffer
        {kTransferUsedDescriptorSetHostDepthBufferBit |
             kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
};

const VulkanRenderTargetCache::TransferModeInfo
    VulkanRenderTargetCache::kTransferModes[size_t(TransferMode::kCount)] = {
        // kColorToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kColor},
        // kColorToColor
        {TransferOutput::kColor, TransferPipelineLayoutIndex::kColor},
        // kDepthToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kDepth},
        // kDepthToColor
        {TransferOutput::kColor, TransferPipelineLayoutIndex::kDepth},
        // kColorToStencilBit
        {TransferOutput::kStencilBit, TransferPipelineLayoutIndex::kColorToStencilBit},
        // kDepthToStencilBit
        {TransferOutput::kStencilBit, TransferPipelineLayoutIndex::kDepthToStencilBit},
        // kColorAndHostDepthToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kColorAndHostDepthTexture},
        // kDepthAndHostDepthToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kDepthAndHostDepthTexture},
        // kColorAndHostDepthCopyToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kColorAndHostDepthBuffer},
        // kDepthAndHostDepthCopyToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kDepthAndHostDepthBuffer},
};

struct VulkanRenderTargetCache::NativeResolveBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize capacity = 0;
  uint64_t last_submission = 0;
  VulkanSharedMemory* shared_memory = nullptr;
  VulkanTextureCache* texture_cache = nullptr;
  SharedMemory::WatchHandle watch = nullptr;
  uint32_t base = 0;
  uint32_t extent_start = 0;
  uint32_t extent_length = 0;
  bool scaled = false;
  bool valid = false;
  bool pending_memory = false;
};

VulkanRenderTargetCache::VulkanRenderTargetCache(const RegisterFile& register_file,
                                                 const memory::Memory& memory,
                                                 TraceWriter& trace_writer,
                                                 uint32_t draw_resolution_scale_x,
                                                 uint32_t draw_resolution_scale_y,
                                                 VulkanCommandProcessor& command_processor)
    : RenderTargetCache(register_file, memory, &trace_writer, draw_resolution_scale_x,
                        draw_resolution_scale_y),
      command_processor_(command_processor),
      memory_(memory),
      trace_writer_(trace_writer) {}

VulkanRenderTargetCache::~VulkanRenderTargetCache() {
  Shutdown(true);
}

bool VulkanRenderTargetCache::Initialize(uint32_t shared_memory_binding_count) {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanInstance::Functions& ifn = vulkan_device->vulkan_instance()->functions();
  const VkPhysicalDevice physical_device = vulkan_device->physical_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();

  bool fsi_path_supported =
      (device_properties.fragmentShaderSampleInterlock ||
       device_properties.fragmentShaderPixelInterlock) &&
      device_properties.fragmentStoresAndAtomics && device_properties.sampleRateShading &&
      device_properties.standardSampleLocations &&
      shared_memory_binding_count < device_properties.maxPerStageDescriptorStorageBuffers;
  const std::string& requested_rt_path = REXCVAR_GET(render_target_path_vulkan);
  if (requested_rt_path == "fsi") {
    path_ = Path::kPixelShaderInterlock;
  } else {
    path_ = Path::kHostRenderTargets;
    native_rt_mode_ = requested_rt_path == "native";
  }
  // Fragment shader interlock is a feature implemented by pretty advanced GPUs,
  // closer to Direct3D 11 / OpenGL ES 3.2 level mainly, not Direct3D 10 /
  // OpenGL ES 3.1. Thus, it's fine to demand a wide range of other optional
  // features for the fragment shader interlock backend to work.
  if (path_ == Path::kPixelShaderInterlock) {
    // Interlocking between fragments with common sample coverage is enough, but
    // interlocking more is acceptable too (fragmentShaderShadingRateInterlock
    // would be okay too, but it's unlikely that an implementation would
    // advertise only it and not any other ones, as it's a very specific feature
    // interacting with another optional feature that is variable shading rate,
    // so there's no need to overcomplicate the checks and the shader execution
    // mode setting).
    // Sample-rate shading is required by certain SPIR-V revisions to access the
    // sample mask fragment shader input.
    // Stanard sample locations are needed for calculating the depth at the
    // samples.
    // It's unlikely that a device exposing fragment shader interlock won't have
    // a large enough storage buffer range and a sufficient SSBO slot count for
    // all the shared memory buffers and the EDRAM buffer - an in a conflict
    // between, for instance, the ability to vfetch and memexport in fragment
    // shaders, and the usage of fragment shader interlock, prefer the former
    // for simplicity.
    if (!fsi_path_supported) {
      path_ = Path::kHostRenderTargets;
    }
  }

  // Format support.
  constexpr VkFormatFeatureFlags kUsedDepthFormatFeatures =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
  constexpr VkFormatFeatureFlags kUsedColorFormatFeatures =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  bool gamma_render_target_as_unorm16_requested =
      path_ == Path::kHostRenderTargets && REXCVAR_GET(gamma_render_target_as_unorm16);
  VkFormatProperties depth_unorm24_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_D24_UNORM_S8_UINT,
                                          &depth_unorm24_properties);
  depth_unorm24_vulkan_format_supported_ = (depth_unorm24_properties.optimalTilingFeatures &
                                            kUsedDepthFormatFeatures) == kUsedDepthFormatFeatures;
  VkFormatProperties color_rg16_snorm_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16_SNORM,
                                          &color_rg16_snorm_properties);
  VkFormatProperties color_rgba16_snorm_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16B16A16_SNORM,
                                          &color_rgba16_snorm_properties);
  VkFormatProperties color_rg16_sfloat_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16_SFLOAT,
                                          &color_rg16_sfloat_properties);
  VkFormatProperties color_rgba16_sfloat_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16B16A16_SFLOAT,
                                          &color_rgba16_sfloat_properties);
  VkFormatProperties color_rgba16_unorm_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16B16A16_UNORM,
                                          &color_rgba16_unorm_properties);
  bool color_rg16_snorm_supported = (color_rg16_snorm_properties.optimalTilingFeatures &
                                     kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
  bool color_rgba16_snorm_supported = (color_rgba16_snorm_properties.optimalTilingFeatures &
                                       kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
  bool color_rg16_sfloat_supported = (color_rg16_sfloat_properties.optimalTilingFeatures &
                                      kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
  bool color_rgba16_sfloat_supported = (color_rgba16_sfloat_properties.optimalTilingFeatures &
                                        kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
  bool color_rgba16_unorm_supported = (color_rgba16_unorm_properties.optimalTilingFeatures &
                                       kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
  bool color_rg16_draw_format_supported = true;
  if (!color_rg16_snorm_supported) {
    if (color_rg16_sfloat_supported) {
      color_rg16_draw_format_fallback_to_float_ = true;
      REXGPU_WARN(
          "VulkanRenderTargetCache: R16G16_SNORM render target support is unavailable; "
          "falling back to R16G16_SFLOAT for k_16_16");
    } else {
      color_rg16_draw_format_supported = false;
    }
  }
  bool color_rgba16_draw_format_supported = true;
  if (!color_rgba16_snorm_supported) {
    if (color_rgba16_sfloat_supported) {
      color_rgba16_draw_format_fallback_to_float_ = true;
      REXGPU_WARN(
          "VulkanRenderTargetCache: R16G16B16A16_SNORM render target support is unavailable; "
          "falling back to R16G16B16A16_SFLOAT for k_16_16_16_16");
    } else {
      color_rgba16_draw_format_supported = false;
    }
  }
  if (path_ == Path::kHostRenderTargets &&
      (!color_rg16_draw_format_supported || !color_rgba16_draw_format_supported)) {
    if (fsi_path_supported) {
      REXGPU_WARN(
          "VulkanRenderTargetCache: Host render target 16-bit formats are unsupported "
          "(R16G16: {}, R16G16B16A16: {}); switching to fragment shader interlock "
          "path for D3D12 parity",
          color_rg16_draw_format_supported ? "available" : "unavailable",
          color_rgba16_draw_format_supported ? "available" : "unavailable");
      path_ = Path::kPixelShaderInterlock;
    } else {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Host render target 16-bit formats are unsupported "
          "(R16G16: {}, R16G16B16A16: {}), and fragment shader interlock fallback "
          "is unavailable",
          color_rg16_draw_format_supported ? "available" : "unavailable",
          color_rgba16_draw_format_supported ? "available" : "unavailable");
      return false;
    }
  }
  if (path_ == Path::kHostRenderTargets && gamma_render_target_as_unorm16_requested &&
      !color_rgba16_unorm_supported) {
    if (fsi_path_supported) {
      REXGPU_WARN(
          "VulkanRenderTargetCache: R16G16B16A16_UNORM render target support "
          "is unavailable for k_8_8_8_8_GAMMA linear storage; switching to "
          "fragment shader interlock path for D3D12 parity");
      path_ = Path::kPixelShaderInterlock;
    } else {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: R16G16B16A16_UNORM render target support "
          "is unavailable for k_8_8_8_8_GAMMA linear storage, and fragment "
          "shader interlock fallback is unavailable");
      return false;
    }
  }
  VkFormatProperties color_rg16_uint_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16_UINT,
                                          &color_rg16_uint_properties);
  VkFormatProperties color_rgba16_uint_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16B16A16_UINT,
                                          &color_rgba16_uint_properties);
  VkFormatProperties color_r32_uint_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R32_UINT,
                                          &color_r32_uint_properties);
  VkFormatProperties color_rg32_uint_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R32G32_UINT,
                                          &color_rg32_uint_properties);
  color_16bit_transfer_uint_formats_supported_ =
      (color_rg16_uint_properties.optimalTilingFeatures & kUsedColorFormatFeatures) ==
          kUsedColorFormatFeatures &&
      (color_rgba16_uint_properties.optimalTilingFeatures & kUsedColorFormatFeatures) ==
          kUsedColorFormatFeatures;
  if (!color_16bit_transfer_uint_formats_supported_) {
    REXGPU_WARN(
        "VulkanRenderTargetCache: R16G16/R16G16B16A16 UINT ownership transfer formats are not "
        "supported");
  }
  color_32bit_transfer_uint_formats_supported_ =
      (color_r32_uint_properties.optimalTilingFeatures & kUsedColorFormatFeatures) ==
          kUsedColorFormatFeatures &&
      (color_rg32_uint_properties.optimalTilingFeatures & kUsedColorFormatFeatures) ==
          kUsedColorFormatFeatures;
  if (!color_32bit_transfer_uint_formats_supported_) {
    REXGPU_WARN(
        "VulkanRenderTargetCache: R32/R32G32 UINT ownership transfer formats are not supported");
  }
  if ((device_properties.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT) &&
      !(device_properties.sampledImageIntegerSampleCounts & VK_SAMPLE_COUNT_4_BIT)) {
    REXGPU_WARN("VulkanRenderTargetCache: 4x integer sampled-image support is unavailable");
  }

  // 2x MSAA support.
  if (REXCVAR_GET(native_2x_msaa)) {
    msaa_2x_attachments_supported_ =
        (device_properties.framebufferColorSampleCounts &
         device_properties.framebufferDepthSampleCounts &
         device_properties.framebufferStencilSampleCounts &
         device_properties.sampledImageColorSampleCounts &
         device_properties.sampledImageDepthSampleCounts &
         device_properties.sampledImageStencilSampleCounts & VK_SAMPLE_COUNT_2_BIT);
    msaa_2x_no_attachments_supported_ =
        (device_properties.framebufferNoAttachmentsSampleCounts & VK_SAMPLE_COUNT_2_BIT) != 0;
  } else {
    msaa_2x_attachments_supported_ = false;
    msaa_2x_no_attachments_supported_ = false;
  }
  bool integer_transfer_sample_1x_supported =
      (device_properties.framebufferColorSampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0 &&
      (device_properties.sampledImageIntegerSampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0;
  bool integer_transfer_sample_2x_supported =
      (device_properties.framebufferColorSampleCounts & VK_SAMPLE_COUNT_2_BIT) != 0 &&
      (device_properties.sampledImageIntegerSampleCounts & VK_SAMPLE_COUNT_2_BIT) != 0;
  bool integer_transfer_sample_4x_supported =
      (device_properties.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT) == 0 ||
      (device_properties.sampledImageIntegerSampleCounts & VK_SAMPLE_COUNT_4_BIT) != 0;
  bool bit_exact_host_color_transfer_supported =
      color_16bit_transfer_uint_formats_supported_ &&
      color_32bit_transfer_uint_formats_supported_ && integer_transfer_sample_1x_supported &&
      integer_transfer_sample_4x_supported &&
      (!msaa_2x_attachments_supported_ || integer_transfer_sample_2x_supported);
  if (path_ == Path::kHostRenderTargets && !bit_exact_host_color_transfer_supported) {
    if (fsi_path_supported) {
      REXGPU_WARN(
          "VulkanRenderTargetCache: Host render target ownership transfers "
          "can't be bit-exact on this device; switching to fragment shader "
          "interlock path for D3D12 parity");
      path_ = Path::kPixelShaderInterlock;
    } else {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Bit-exact host render target ownership "
          "transfers require UINT transfer formats and integer sampled-image "
          "MSAA support, and fragment shader interlock fallback is unavailable");
      return false;
    }
  }

  // A device-support fallback above may have moved us off the host path after
  // "native" was requested; native mode is meaningless then, so don't claim it.
  if (path_ != Path::kHostRenderTargets) {
    native_rt_mode_ = false;
  }
  msaa_as_single_sample_ = native_rt_mode_ && REXCVAR_GET(native_rt_msaa_as_single_sample);
  REXGPU_INFO("VulkanRenderTargetCache: render target path = {}{}",
              path_ == Path::kPixelShaderInterlock ? "fragment shader interlock"
              : native_rt_mode_                    ? "native (host render targets)"
                                                   : "host render targets",
              msaa_as_single_sample_ ? ", MSAA surfaces single-sampled" : "");

  // Descriptor set layouts.
  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings[2];
  descriptor_set_layout_bindings[0].binding = 0;
  descriptor_set_layout_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_set_layout_bindings[0].descriptorCount = 1;
  descriptor_set_layout_bindings[0].stageFlags =
      VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_bindings[0].pImmutableSamplers = nullptr;
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = 1;
  descriptor_set_layout_create_info.pBindings = descriptor_set_layout_bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_storage_buffer_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the descriptor set layout "
        "with one storage buffer");
    Shutdown();
    return false;
  }
  descriptor_set_layout_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_sampled_image_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the descriptor set layout "
        "with one sampled image");
    Shutdown();
    return false;
  }
  descriptor_set_layout_bindings[1].binding = 1;
  descriptor_set_layout_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptor_set_layout_bindings[1].descriptorCount = 1;
  descriptor_set_layout_bindings[1].stageFlags = descriptor_set_layout_bindings[0].stageFlags;
  descriptor_set_layout_bindings[1].pImmutableSamplers = nullptr;
  descriptor_set_layout_create_info.bindingCount = 2;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_sampled_image_x2_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the descriptor set layout "
        "with two sampled images");
    Shutdown();
    return false;
  }

  // Descriptor set pools.
  // The pool sizes were chosen without a specific reason.
  VkDescriptorPoolSize descriptor_set_layout_size;
  descriptor_set_layout_size.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptor_set_layout_size.descriptorCount = 1;
  descriptor_set_pool_sampled_image_ = std::make_unique<ui::vulkan::SingleLayoutDescriptorSetPool>(
      vulkan_device, 256, 1, &descriptor_set_layout_size, descriptor_set_layout_sampled_image_);
  descriptor_set_layout_size.descriptorCount = 2;
  descriptor_set_pool_sampled_image_x2_ =
      std::make_unique<ui::vulkan::SingleLayoutDescriptorSetPool>(
          vulkan_device, 256, 1, &descriptor_set_layout_size,
          descriptor_set_layout_sampled_image_x2_);

  // EDRAM contents reinterpretation buffer.
  // 90 MB with 9x resolution scaling - within the minimum
  // maxStorageBufferRange.
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device,
          VkDeviceSize(xenos::kEdramSizeBytes *
                       (draw_resolution_scale_x() * draw_resolution_scale_y())),
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, edram_buffer_, edram_buffer_memory_)) {
    REXGPU_ERROR("VulkanRenderTargetCache: Failed to create the EDRAM buffer");
    Shutdown();
    return false;
  }
  if (GetPath() == Path::kPixelShaderInterlock) {
    // The first operation will likely be drawing.
    edram_buffer_usage_ = EdramBufferUsage::kFragmentReadWrite;
  } else {
    // The first operation will likely be depth self-comparison.
    edram_buffer_usage_ = EdramBufferUsage::kFragmentRead;
  }
  edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
  VkDescriptorPoolSize edram_storage_buffer_descriptor_pool_size;
  edram_storage_buffer_descriptor_pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  edram_storage_buffer_descriptor_pool_size.descriptorCount = 1;
  VkDescriptorPoolCreateInfo edram_storage_buffer_descriptor_pool_create_info;
  edram_storage_buffer_descriptor_pool_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  edram_storage_buffer_descriptor_pool_create_info.pNext = nullptr;
  edram_storage_buffer_descriptor_pool_create_info.flags = 0;
  edram_storage_buffer_descriptor_pool_create_info.maxSets = 1;
  edram_storage_buffer_descriptor_pool_create_info.poolSizeCount = 1;
  edram_storage_buffer_descriptor_pool_create_info.pPoolSizes =
      &edram_storage_buffer_descriptor_pool_size;
  if (dfn.vkCreateDescriptorPool(device, &edram_storage_buffer_descriptor_pool_create_info, nullptr,
                                 &edram_storage_buffer_descriptor_pool_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the EDRAM buffer storage "
        "buffer descriptor pool");
    Shutdown();
    return false;
  }
  VkDescriptorSetAllocateInfo edram_storage_buffer_descriptor_set_allocate_info;
  edram_storage_buffer_descriptor_set_allocate_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  edram_storage_buffer_descriptor_set_allocate_info.pNext = nullptr;
  edram_storage_buffer_descriptor_set_allocate_info.descriptorPool =
      edram_storage_buffer_descriptor_pool_;
  edram_storage_buffer_descriptor_set_allocate_info.descriptorSetCount = 1;
  edram_storage_buffer_descriptor_set_allocate_info.pSetLayouts =
      &descriptor_set_layout_storage_buffer_;
  if (dfn.vkAllocateDescriptorSets(device, &edram_storage_buffer_descriptor_set_allocate_info,
                                   &edram_storage_buffer_descriptor_set_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to allocate the EDRAM buffer storage "
        "buffer descriptor set");
    Shutdown();
    return false;
  }
  VkDescriptorBufferInfo edram_storage_buffer_descriptor_buffer_info;
  edram_storage_buffer_descriptor_buffer_info.buffer = edram_buffer_;
  edram_storage_buffer_descriptor_buffer_info.offset = 0;
  edram_storage_buffer_descriptor_buffer_info.range = VK_WHOLE_SIZE;
  VkWriteDescriptorSet edram_storage_buffer_descriptor_write;
  edram_storage_buffer_descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  edram_storage_buffer_descriptor_write.pNext = nullptr;
  edram_storage_buffer_descriptor_write.dstSet = edram_storage_buffer_descriptor_set_;
  edram_storage_buffer_descriptor_write.dstBinding = 0;
  edram_storage_buffer_descriptor_write.dstArrayElement = 0;
  edram_storage_buffer_descriptor_write.descriptorCount = 1;
  edram_storage_buffer_descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  edram_storage_buffer_descriptor_write.pImageInfo = nullptr;
  edram_storage_buffer_descriptor_write.pBufferInfo = &edram_storage_buffer_descriptor_buffer_info;
  edram_storage_buffer_descriptor_write.pTexelBufferView = nullptr;
  dfn.vkUpdateDescriptorSets(device, 1, &edram_storage_buffer_descriptor_write, 0, nullptr);

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  // Resolve copy pipeline layout.
  VkDescriptorSetLayout resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetCount] = {};
  resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetEdram] =
      descriptor_set_layout_storage_buffer_;
  resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetDest] =
      command_processor_.GetSingleTransientDescriptorLayout(
          VulkanCommandProcessor::SingleTransientDescriptorLayout ::kStorageBufferCompute);
  VkPushConstantRange resolve_copy_push_constant_range;
  resolve_copy_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  resolve_copy_push_constant_range.offset = 0;
  // Potentially binding all of the shared memory at 1x resolution, but only
  // portions with scaled resolution.
  resolve_copy_push_constant_range.size =
      draw_resolution_scaled ? sizeof(draw_util::ResolveCopyShaderConstants::DestRelative)
                             : sizeof(draw_util::ResolveCopyShaderConstants);
  VkPipelineLayoutCreateInfo resolve_copy_pipeline_layout_create_info;
  resolve_copy_pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  resolve_copy_pipeline_layout_create_info.pNext = nullptr;
  resolve_copy_pipeline_layout_create_info.flags = 0;
  resolve_copy_pipeline_layout_create_info.setLayoutCount = kResolveCopyDescriptorSetCount;
  resolve_copy_pipeline_layout_create_info.pSetLayouts = resolve_copy_descriptor_set_layouts;
  resolve_copy_pipeline_layout_create_info.pushConstantRangeCount = 1;
  resolve_copy_pipeline_layout_create_info.pPushConstantRanges = &resolve_copy_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &resolve_copy_pipeline_layout_create_info, nullptr,
                                 &resolve_copy_pipeline_layout_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the resolve copy pipeline "
        "layout");
    Shutdown();
    return false;
  }

  // Direct resolve pipeline layouts (destination storage buffer + source image).
  auto create_direct_resolve_pipeline_layout = [&](VkDescriptorSetLayout source_layout,
                                                   VkPipelineLayout* pipeline_layout_out) {
    VkDescriptorSetLayout descriptor_set_layouts[] = {
        command_processor_.GetSingleTransientDescriptorLayout(
            VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferCompute),
        source_layout,
    };
    VkPushConstantRange push_constant_range;
    push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_constant_range.offset = 0;
    push_constant_range.size = sizeof(DirectResolvePushConstants);
    VkPipelineLayoutCreateInfo pipeline_layout_create_info;
    pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.pNext = nullptr;
    pipeline_layout_create_info.flags = 0;
    pipeline_layout_create_info.setLayoutCount = uint32_t(rex::countof(descriptor_set_layouts));
    pipeline_layout_create_info.pSetLayouts = descriptor_set_layouts;
    pipeline_layout_create_info.pushConstantRangeCount = 1;
    pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr,
                                   pipeline_layout_out) != VK_SUCCESS) {
      *pipeline_layout_out = VK_NULL_HANDLE;
    }
  };
  create_direct_resolve_pipeline_layout(descriptor_set_layout_sampled_image_,
                                        &direct_resolve_pipeline_layout_color_);
  create_direct_resolve_pipeline_layout(descriptor_set_layout_sampled_image_x2_,
                                        &direct_resolve_pipeline_layout_depth_);

  // Resolve copy pipelines.
  for (size_t i = 0; i < size_t(draw_util::ResolveCopyShaderIndex::kCount); ++i) {
    const draw_util::ResolveCopyShaderInfo& resolve_copy_shader_info =
        draw_util::resolve_copy_shader_info[i];
    const ResolveCopyShaderCode& resolve_copy_shader_code = kResolveCopyShaders[i];
    // Somewhat verification whether resolve_copy_shaders_ is up to date.
    assert_true(resolve_copy_shader_code.unscaled && resolve_copy_shader_code.unscaled_size_bytes &&
                resolve_copy_shader_code.scaled && resolve_copy_shader_code.scaled_size_bytes);
    VkPipeline resolve_copy_pipeline = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_copy_pipeline_layout_,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled
                               : resolve_copy_shader_code.unscaled,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled_size_bytes
                               : resolve_copy_shader_code.unscaled_size_bytes);
    if (resolve_copy_pipeline == VK_NULL_HANDLE) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the resolve copy "
          "pipeline {}",
          resolve_copy_shader_info.debug_name);
      Shutdown();
      return false;
    }
    vulkan_device->SetObjectName(VK_OBJECT_TYPE_PIPELINE, resolve_copy_pipeline,
                                 resolve_copy_shader_info.debug_name);
    resolve_copy_pipelines_[i] = resolve_copy_pipeline;
  }

  if (path_ == Path::kHostRenderTargets) {
    // Host render targets.

    gamma_render_target_as_unorm16_ = gamma_render_target_as_unorm16_requested;

    depth_float24_round_ = REXCVAR_GET(depth_float24_round);
    depth_float24_convert_in_pixel_shader_ = REXCVAR_GET(depth_float24_convert_in_pixel_shader);

    // Host depth storing pipeline layout.
    VkDescriptorSetLayout host_depth_store_descriptor_set_layouts[] = {
        // Destination EDRAM storage buffer.
        descriptor_set_layout_storage_buffer_,
        // Source depth / stencil texture (only depth is used).
        descriptor_set_layout_sampled_image_x2_,
    };
    VkPushConstantRange host_depth_store_push_constant_range;
    host_depth_store_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    host_depth_store_push_constant_range.offset = 0;
    host_depth_store_push_constant_range.size = sizeof(HostDepthStoreConstants);
    VkPipelineLayoutCreateInfo host_depth_store_pipeline_layout_create_info;
    host_depth_store_pipeline_layout_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    host_depth_store_pipeline_layout_create_info.pNext = nullptr;
    host_depth_store_pipeline_layout_create_info.flags = 0;
    host_depth_store_pipeline_layout_create_info.setLayoutCount =
        uint32_t(rex::countof(host_depth_store_descriptor_set_layouts));
    host_depth_store_pipeline_layout_create_info.pSetLayouts =
        host_depth_store_descriptor_set_layouts;
    host_depth_store_pipeline_layout_create_info.pushConstantRangeCount = 1;
    host_depth_store_pipeline_layout_create_info.pPushConstantRanges =
        &host_depth_store_push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &host_depth_store_pipeline_layout_create_info, nullptr,
                                   &host_depth_store_pipeline_layout_) != VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the host depth storing "
          "pipeline layout");
      Shutdown();
      return false;
    }
    const std::pair<const uint32_t*, size_t> host_depth_store_shaders[] = {
        {shaders::host_depth_store_1xmsaa_cs, sizeof(shaders::host_depth_store_1xmsaa_cs)},
        {shaders::host_depth_store_2xmsaa_cs, sizeof(shaders::host_depth_store_2xmsaa_cs)},
        {shaders::host_depth_store_4xmsaa_cs, sizeof(shaders::host_depth_store_4xmsaa_cs)},
    };
    for (size_t i = 0; i < rex::countof(host_depth_store_shaders); ++i) {
      const std::pair<const uint32_t*, size_t> host_depth_store_shader =
          host_depth_store_shaders[i];
      VkPipeline host_depth_store_pipeline = ui::vulkan::util::CreateComputePipeline(
          vulkan_device, host_depth_store_pipeline_layout_, host_depth_store_shader.first,
          host_depth_store_shader.second);
      if (host_depth_store_pipeline == VK_NULL_HANDLE) {
        REXGPU_ERROR(
            "VulkanRenderTargetCache: Failed to create the {}-sample host "
            "depth storing pipeline",
            uint32_t(1) << i);
        Shutdown();
        return false;
      }
      host_depth_store_pipelines_[i] = host_depth_store_pipeline;
    }

    // Transfer and clear vertex buffer, for quads of up to tile granularity.
    transfer_vertex_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
        vulkan_device, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        std::max(
            ui::vulkan::VulkanUploadBufferPool::kDefaultPageSize,
            sizeof(float) * 2 * 6 * Transfer::kMaxCutoutBorderRectangles * xenos::kEdramTileCount));

    // Transfer vertex shader.
    transfer_passthrough_vertex_shader_ =
        ui::vulkan::util::CreateShaderModule(vulkan_device, shaders::passthrough_position_xy_vs,
                                             sizeof(shaders::passthrough_position_xy_vs));
    if (transfer_passthrough_vertex_shader_ == VK_NULL_HANDLE) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the render target "
          "ownership transfer vertex shader");
      Shutdown();
      return false;
    }

    // Transfer pipeline layouts.
    VkDescriptorSetLayout
        transfer_pipeline_layout_descriptor_set_layouts[kTransferUsedDescriptorSetCount];
    VkPushConstantRange transfer_pipeline_layout_push_constant_range;
    transfer_pipeline_layout_push_constant_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    transfer_pipeline_layout_push_constant_range.offset = 0;
    VkPipelineLayoutCreateInfo transfer_pipeline_layout_create_info;
    transfer_pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    transfer_pipeline_layout_create_info.pNext = nullptr;
    transfer_pipeline_layout_create_info.flags = 0;
    transfer_pipeline_layout_create_info.pSetLayouts =
        transfer_pipeline_layout_descriptor_set_layouts;
    transfer_pipeline_layout_create_info.pPushConstantRanges =
        &transfer_pipeline_layout_push_constant_range;
    for (size_t i = 0; i < size_t(TransferPipelineLayoutIndex::kCount); ++i) {
      const TransferPipelineLayoutInfo& transfer_pipeline_layout_info =
          kTransferPipelineLayoutInfos[i];
      transfer_pipeline_layout_create_info.setLayoutCount = 0;
      uint32_t transfer_pipeline_layout_descriptor_sets_remaining =
          transfer_pipeline_layout_info.used_descriptor_sets;
      uint32_t transfer_pipeline_layout_descriptor_set_index;
      while (rex::bit_scan_forward(transfer_pipeline_layout_descriptor_sets_remaining,
                                   &transfer_pipeline_layout_descriptor_set_index)) {
        transfer_pipeline_layout_descriptor_sets_remaining &=
            ~(uint32_t(1) << transfer_pipeline_layout_descriptor_set_index);
        VkDescriptorSetLayout transfer_pipeline_layout_descriptor_set_layout = VK_NULL_HANDLE;
        switch (TransferUsedDescriptorSet(transfer_pipeline_layout_descriptor_set_index)) {
          case kTransferUsedDescriptorSetHostDepthBuffer:
            transfer_pipeline_layout_descriptor_set_layout = descriptor_set_layout_storage_buffer_;
            break;
          case kTransferUsedDescriptorSetHostDepthStencilTextures:
          case kTransferUsedDescriptorSetDepthStencilTextures:
            transfer_pipeline_layout_descriptor_set_layout =
                descriptor_set_layout_sampled_image_x2_;
            break;
          case kTransferUsedDescriptorSetColorTexture:
            transfer_pipeline_layout_descriptor_set_layout = descriptor_set_layout_sampled_image_;
            break;
          default:
            assert_unhandled_case(
                TransferUsedDescriptorSet(transfer_pipeline_layout_descriptor_set_index));
        }
        transfer_pipeline_layout_descriptor_set_layouts[transfer_pipeline_layout_create_info
                                                            .setLayoutCount++] =
            transfer_pipeline_layout_descriptor_set_layout;
      }
      transfer_pipeline_layout_push_constant_range.size =
          uint32_t(sizeof(uint32_t) *
                   rex::bit_count(transfer_pipeline_layout_info.used_push_constant_dwords));
      transfer_pipeline_layout_create_info.pushConstantRangeCount =
          transfer_pipeline_layout_info.used_push_constant_dwords ? 1 : 0;
      if (dfn.vkCreatePipelineLayout(device, &transfer_pipeline_layout_create_info, nullptr,
                                     &transfer_pipeline_layouts_[i]) != VK_SUCCESS) {
        REXGPU_ERROR(
            "VulkanRenderTargetCache: Failed to create the render target "
            "ownership transfer pipeline layout {}",
            i);
        Shutdown();
        return false;
      }
    }

    // Dump pipeline layouts.
    VkDescriptorSetLayout dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetCount];
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetEdram] =
        descriptor_set_layout_storage_buffer_;
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetSource] =
        descriptor_set_layout_sampled_image_;
    VkPushConstantRange dump_pipeline_layout_push_constant_range;
    dump_pipeline_layout_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    dump_pipeline_layout_push_constant_range.offset = 0;
    dump_pipeline_layout_push_constant_range.size = sizeof(uint32_t) * kDumpPushConstantCount;
    VkPipelineLayoutCreateInfo dump_pipeline_layout_create_info;
    dump_pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    dump_pipeline_layout_create_info.pNext = nullptr;
    dump_pipeline_layout_create_info.flags = 0;
    dump_pipeline_layout_create_info.setLayoutCount =
        uint32_t(rex::countof(dump_pipeline_layout_descriptor_set_layouts));
    dump_pipeline_layout_create_info.pSetLayouts = dump_pipeline_layout_descriptor_set_layouts;
    dump_pipeline_layout_create_info.pushConstantRangeCount = 1;
    dump_pipeline_layout_create_info.pPushConstantRanges =
        &dump_pipeline_layout_push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &dump_pipeline_layout_create_info, nullptr,
                                   &dump_pipeline_layout_color_) != VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the color render target "
          "dumping pipeline layout");
      Shutdown();
      return false;
    }
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetSource] =
        descriptor_set_layout_sampled_image_x2_;
    if (dfn.vkCreatePipelineLayout(device, &dump_pipeline_layout_create_info, nullptr,
                                   &dump_pipeline_layout_depth_) != VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the depth render target "
          "dumping pipeline layout");
      Shutdown();
      return false;
    }
  } else if (path_ == Path::kPixelShaderInterlock) {
    // Pixel (fragment) shader interlock.

    // Keep parity with D3D12 ROV, which uses 2x-as-4x in this path.
    msaa_2x_attachments_supported_ = false;
    msaa_2x_no_attachments_supported_ = false;

    // Blending is done in linear space directly in shaders.
    gamma_render_target_as_unorm16_ = false;

    // Always true float24 depth rounded to the nearest even.
    depth_float24_round_ = true;
    depth_float24_convert_in_pixel_shader_ = true;

    // The pipeline layout and the pipelines for clearing the EDRAM buffer in
    // resolves.
    VkPushConstantRange resolve_fsi_clear_push_constant_range;
    resolve_fsi_clear_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_fsi_clear_push_constant_range.offset = 0;
    resolve_fsi_clear_push_constant_range.size = sizeof(draw_util::ResolveClearShaderConstants);
    VkPipelineLayoutCreateInfo resolve_fsi_clear_pipeline_layout_create_info;
    resolve_fsi_clear_pipeline_layout_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    resolve_fsi_clear_pipeline_layout_create_info.pNext = nullptr;
    resolve_fsi_clear_pipeline_layout_create_info.flags = 0;
    resolve_fsi_clear_pipeline_layout_create_info.setLayoutCount = 1;
    resolve_fsi_clear_pipeline_layout_create_info.pSetLayouts =
        &descriptor_set_layout_storage_buffer_;
    resolve_fsi_clear_pipeline_layout_create_info.pushConstantRangeCount = 1;
    resolve_fsi_clear_pipeline_layout_create_info.pPushConstantRanges =
        &resolve_fsi_clear_push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &resolve_fsi_clear_pipeline_layout_create_info, nullptr,
                                   &resolve_fsi_clear_pipeline_layout_) != VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the resolve EDRAM buffer "
          "clear pipeline layout");
      Shutdown();
      return false;
    }
    resolve_fsi_clear_32bpp_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_fsi_clear_pipeline_layout_,
        draw_resolution_scaled ? shaders::resolve_clear_32bpp_scaled_cs
                               : shaders::resolve_clear_32bpp_cs,
        draw_resolution_scaled ? sizeof(shaders::resolve_clear_32bpp_scaled_cs)
                               : sizeof(shaders::resolve_clear_32bpp_cs));
    if (resolve_fsi_clear_32bpp_pipeline_ == VK_NULL_HANDLE) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the 32bpp resolve EDRAM "
          "buffer clear pipeline");
      Shutdown();
      return false;
    }
    resolve_fsi_clear_64bpp_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_fsi_clear_pipeline_layout_,
        draw_resolution_scaled ? shaders::resolve_clear_64bpp_scaled_cs
                               : shaders::resolve_clear_64bpp_cs,
        draw_resolution_scaled ? sizeof(shaders::resolve_clear_64bpp_scaled_cs)
                               : sizeof(shaders::resolve_clear_64bpp_cs));
    if (resolve_fsi_clear_64bpp_pipeline_ == VK_NULL_HANDLE) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the 64bpp resolve EDRAM "
          "buffer clear pipeline");
      Shutdown();
      return false;
    }

    // Common render pass.
    VkSubpassDescription fsi_subpass = {};
    fsi_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    // Fragment shader interlock provides synchronization and ordering within a
    // subpass, create an external by-region dependency to maintain interlocking
    // between passes. Framebuffer-global dependencies will be made with
    // explicit barriers when the addressing of the EDRAM buffer relatively to
    // the fragment coordinates is changed.
    VkSubpassDependency fsi_subpass_dependencies[2];
    fsi_subpass_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    fsi_subpass_dependencies[0].dstSubpass = 0;
    fsi_subpass_dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    fsi_subpass_dependencies[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    fsi_subpass_dependencies[0].srcAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    fsi_subpass_dependencies[0].dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    fsi_subpass_dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    fsi_subpass_dependencies[1] = fsi_subpass_dependencies[0];
    std::swap(fsi_subpass_dependencies[1].srcSubpass, fsi_subpass_dependencies[1].dstSubpass);
    VkRenderPassCreateInfo fsi_render_pass_create_info;
    fsi_render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    fsi_render_pass_create_info.pNext = nullptr;
    fsi_render_pass_create_info.flags = 0;
    fsi_render_pass_create_info.attachmentCount = 0;
    fsi_render_pass_create_info.pAttachments = nullptr;
    fsi_render_pass_create_info.subpassCount = 1;
    fsi_render_pass_create_info.pSubpasses = &fsi_subpass;
    fsi_render_pass_create_info.dependencyCount = uint32_t(rex::countof(fsi_subpass_dependencies));
    fsi_render_pass_create_info.pDependencies = fsi_subpass_dependencies;
    if (dfn.vkCreateRenderPass(device, &fsi_render_pass_create_info, nullptr, &fsi_render_pass_) !=
        VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the fragment shader "
          "interlock render backend render pass");
      Shutdown();
      return false;
    }

    // Common framebuffer.
    VkFramebufferCreateInfo fsi_framebuffer_create_info;
    fsi_framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fsi_framebuffer_create_info.pNext = nullptr;
    fsi_framebuffer_create_info.flags = 0;
    fsi_framebuffer_create_info.renderPass = fsi_render_pass_;
    fsi_framebuffer_create_info.attachmentCount = 0;
    fsi_framebuffer_create_info.pAttachments = nullptr;
    fsi_framebuffer_create_info.width =
        std::min(xenos::kTexture2DCubeMaxWidthHeight * draw_resolution_scale_x(),
                 device_properties.maxFramebufferWidth);
    fsi_framebuffer_create_info.height =
        std::min(xenos::kTexture2DCubeMaxWidthHeight * draw_resolution_scale_y(),
                 device_properties.maxFramebufferHeight);
    fsi_framebuffer_create_info.layers = 1;
    if (dfn.vkCreateFramebuffer(device, &fsi_framebuffer_create_info, nullptr,
                                &fsi_framebuffer_.framebuffer) != VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the fragment shader "
          "interlock render backend framebuffer");
      Shutdown();
      return false;
    }
    fsi_framebuffer_.host_extent.width = fsi_framebuffer_create_info.width;
    fsi_framebuffer_.host_extent.height = fsi_framebuffer_create_info.height;
  } else {
    assert_unhandled_case(path_);
    Shutdown();
    return false;
  }

  // Reset the last update structures, to keep the defaults consistent between
  // paths regardless of whether the update for the path actually modifies them.
  last_update_render_pass_key_ = RenderPassKey();
  last_update_render_pass_ = VK_NULL_HANDLE;
  last_update_framebuffer_pitch_tiles_at_32bpp_ = 0;
  std::memset(last_update_framebuffer_attachments_, 0,
              sizeof(last_update_framebuffer_attachments_));
  last_update_framebuffer_ = VK_NULL_HANDLE;

  if (path_ == Path::kHostRenderTargets && native_rt_mode_ && REXCVAR_GET(native_resolve)) {
    native_resolve_enabled_ = InitializeNativeResolve(shared_memory_binding_count);
  }

  InitializeCommon();
  return true;
}

void VulkanRenderTargetCache::Shutdown(bool from_destructor) {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  pending_copy_free_resolve_.active = false;
  ResetTraceDownload();
  ShutdownNativeResolve();
  ClearNativeResolveBuffers();

  // Destroy all render targets before the descriptor set pool is destroyed -
  // may happen if shutting down the VulkanRenderTargetCache by destroying it,
  // so ShutdownCommon is called by the RenderTargetCache destructor, when it's
  // already too late.
  DestroyRetiredRenderTargetObjects(UINT64_MAX);
  DestroyAllRenderTargets(true);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         resolve_fsi_clear_64bpp_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         resolve_fsi_clear_32bpp_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_fsi_clear_pipeline_layout_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                         fsi_framebuffer_.framebuffer);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device, fsi_render_pass_);

  for (const auto& dump_pipeline_pair : dump_pipelines_) {
    // May be null to prevent recreation attempts.
    if (dump_pipeline_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, dump_pipeline_pair.second, nullptr);
    }
  }
  dump_pipelines_.clear();
  for (const auto& direct_resolve_pipeline_pair : direct_resolve_pipelines_) {
    bool aliased_resolve_copy_pipeline = false;
    for (VkPipeline resolve_copy_pipeline : resolve_copy_pipelines_) {
      if (direct_resolve_pipeline_pair.second == resolve_copy_pipeline) {
        aliased_resolve_copy_pipeline = true;
        break;
      }
    }
    if (direct_resolve_pipeline_pair.second != VK_NULL_HANDLE && !aliased_resolve_copy_pipeline) {
      dfn.vkDestroyPipeline(device, direct_resolve_pipeline_pair.second, nullptr);
    }
  }
  direct_resolve_pipelines_.clear();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         direct_resolve_pipeline_layout_depth_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         direct_resolve_pipeline_layout_color_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         dump_pipeline_layout_depth_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         dump_pipeline_layout_color_);

  for (const auto& transfer_pipeline_array_pair : transfer_pipelines_) {
    for (VkPipeline transfer_pipeline : transfer_pipeline_array_pair.second) {
      // May be null to prevent recreation attempts.
      if (transfer_pipeline != VK_NULL_HANDLE) {
        dfn.vkDestroyPipeline(device, transfer_pipeline, nullptr);
      }
    }
  }
  transfer_pipelines_.clear();
  for (const auto& transfer_shader_pair : transfer_shaders_) {
    if (transfer_shader_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, transfer_shader_pair.second, nullptr);
    }
  }
  transfer_shaders_.clear();
  for (size_t i = 0; i < size_t(TransferPipelineLayoutIndex::kCount); ++i) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                           transfer_pipeline_layouts_[i]);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         transfer_passthrough_vertex_shader_);
  transfer_vertex_buffer_pool_.reset();
  edram_snapshot_restore_pool_.reset();

  for (size_t i = 0; i < rex::countof(host_depth_store_pipelines_); ++i) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                           host_depth_store_pipelines_[i]);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         host_depth_store_pipeline_layout_);

  last_update_framebuffer_ = VK_NULL_HANDLE;
  for (const auto& framebuffer_pair : framebuffers_) {
    dfn.vkDestroyFramebuffer(device, framebuffer_pair.second.framebuffer, nullptr);
  }
  framebuffers_.clear();

  last_update_render_pass_ = VK_NULL_HANDLE;
  for (const auto& render_pass_pair : render_passes_) {
    if (render_pass_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyRenderPass(device, render_pass_pair.second, nullptr);
    }
  }
  render_passes_.clear();

  for (VkPipeline& resolve_copy_pipeline : resolve_copy_pipelines_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, resolve_copy_pipeline);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_copy_pipeline_layout_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         edram_storage_buffer_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, edram_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, edram_buffer_memory_);

  descriptor_set_pool_sampled_image_x2_.reset();
  descriptor_set_pool_sampled_image_.reset();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         descriptor_set_layout_sampled_image_x2_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         descriptor_set_layout_sampled_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         descriptor_set_layout_storage_buffer_);

  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanRenderTargetCache::ClearCache() {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Held back transfers and resolves reference render targets the common
  // ClearCache destroys.
  pending_copy_free_resolve_.active = false;
  deferred_transfer_targets_ = 0;
  for (std::vector<Transfer>& transfers : deferred_transfers_) {
    transfers.clear();
  }
  std::memset(deferred_transfer_bindings_, 0, sizeof(deferred_transfer_bindings_));

  // Called with the GPU idle.
  DestroyRetiredRenderTargetObjects(UINT64_MAX);
  ClearNativeResolveBuffers();

  // Framebuffer objects must be destroyed because they reference views of
  // attachment images, which may be removed by the common ClearCache.
  last_update_framebuffer_ = VK_NULL_HANDLE;
  for (const auto& framebuffer_pair : framebuffers_) {
    dfn.vkDestroyFramebuffer(device, framebuffer_pair.second.framebuffer, nullptr);
  }
  framebuffers_.clear();

  last_update_render_pass_ = VK_NULL_HANDLE;
  for (const auto& render_pass_pair : render_passes_) {
    dfn.vkDestroyRenderPass(device, render_pass_pair.second, nullptr);
  }
  render_passes_.clear();

  RenderTargetCache::ClearCache();
}

void VulkanRenderTargetCache::CompletedSubmissionUpdated() {
  DestroyRetiredRenderTargetObjects(command_processor_.GetCompletedSubmission());
  if (edram_snapshot_restore_pool_) {
    edram_snapshot_restore_pool_->Reclaim(command_processor_.GetCompletedSubmission());
  }
  if (transfer_vertex_buffer_pool_) {
    transfer_vertex_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
  }
}

void VulkanRenderTargetCache::EndSubmission() {
  if (edram_snapshot_restore_pool_) {
    edram_snapshot_restore_pool_->FlushWrites();
  }
  if (transfer_vertex_buffer_pool_) {
    transfer_vertex_buffer_pool_->FlushWrites();
  }
}

void VulkanRenderTargetCache::ResetTraceDownload() {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         edram_snapshot_download_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         edram_snapshot_download_buffer_memory_);
  edram_snapshot_download_buffer_memory_type_ = UINT32_MAX;
  edram_snapshot_download_buffer_memory_size_ = 0;
}

bool VulkanRenderTargetCache::InitializeTraceSubmitDownloads() {
  ResetTraceDownload();

  if (IsDrawResolutionScaled()) {
    // No 1:1 mapping.
    return false;
  }

  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          command_processor_.GetVulkanDevice(), xenos::kEdramSizeBytes,
          VK_BUFFER_USAGE_TRANSFER_DST_BIT, ui::vulkan::util::MemoryPurpose::kReadback,
          edram_snapshot_download_buffer_, edram_snapshot_download_buffer_memory_,
          &edram_snapshot_download_buffer_memory_type_,
          &edram_snapshot_download_buffer_memory_size_)) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create an EDRAM snapshot download "
        "buffer");
    ResetTraceDownload();
    return false;
  }

  if (GetPath() == Path::kHostRenderTargets) {
    // Dump all host render targets to edram_buffer_.
    if (!DumpRenderTargets(0, xenos::kEdramTileCount, 1, xenos::kEdramTileCount)) {
      REXGPU_ERROR("VulkanRenderTargetCache: Failed to dump host render targets for trace");
      ResetTraceDownload();
      return false;
    }
  }

  UseEdramBuffer(EdramBufferUsage::kTransferRead);
  command_processor_.SubmitBarriers(true);
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  VkBufferCopy edram_download_copy;
  edram_download_copy.srcOffset = 0;
  edram_download_copy.dstOffset = 0;
  edram_download_copy.size = xenos::kEdramSizeBytes;
  command_buffer.CmdVkCopyBuffer(edram_buffer_, edram_snapshot_download_buffer_, 1,
                                 &edram_download_copy);
  command_processor_.PushBufferMemoryBarrier(
      edram_snapshot_download_buffer_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
  return true;
}

void VulkanRenderTargetCache::InitializeTraceCompleteDownloads() {
  if (edram_snapshot_download_buffer_memory_ == VK_NULL_HANDLE) {
    return;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  void* edram_snapshot_download_mapping = nullptr;
  if (dfn.vkMapMemory(device, edram_snapshot_download_buffer_memory_, 0, VK_WHOLE_SIZE, 0,
                      &edram_snapshot_download_mapping) == VK_SUCCESS) {
    if (!(vulkan_device->memory_types().host_coherent &
          (uint32_t(1) << edram_snapshot_download_buffer_memory_type_))) {
      VkMappedMemoryRange edram_snapshot_download_memory_range = {};
      edram_snapshot_download_memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      edram_snapshot_download_memory_range.memory = edram_snapshot_download_buffer_memory_;
      edram_snapshot_download_memory_range.offset = 0;
      edram_snapshot_download_memory_range.size =
          std::min(rex::round_up(VkDeviceSize(xenos::kEdramSizeBytes),
                                 vulkan_device->properties().nonCoherentAtomSize),
                   edram_snapshot_download_buffer_memory_size_);
      dfn.vkInvalidateMappedMemoryRanges(device, 1, &edram_snapshot_download_memory_range);
    }

    trace_writer_.WriteEdramSnapshot(edram_snapshot_download_mapping);
    dfn.vkUnmapMemory(device, edram_snapshot_download_buffer_memory_);
  } else {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to map the EDRAM snapshot download "
        "buffer");
  }

  ResetTraceDownload();
}

void VulkanRenderTargetCache::RestoreEdramSnapshot(const void* snapshot) {
  FlushDeferredTransfers();
  ++uniform_stencil_generation_;
  if (IsDrawResolutionScaled()) {
    // No 1:1 mapping.
    return;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  if (!edram_snapshot_restore_pool_) {
    edram_snapshot_restore_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
        vulkan_device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, xenos::kEdramSizeBytes);
  }
  VkBuffer upload_buffer;
  VkDeviceSize upload_buffer_offset;
  uint8_t* upload_buffer_mapping = edram_snapshot_restore_pool_->Request(
      command_processor_.GetCurrentSubmission(), xenos::kEdramSizeBytes, 1, upload_buffer,
      upload_buffer_offset);
  if (!upload_buffer_mapping) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to get a buffer for restoring an "
        "EDRAM snapshot");
    return;
  }

  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();

  switch (GetPath()) {
    case Path::kHostRenderTargets: {
      // k_32_FLOAT because it's unambiguous.
      VulkanRenderTarget* full_edram_render_target =
          static_cast<VulkanRenderTarget*>(PrepareFullEdram1280xRenderTargetForSnapshotRestoration(
              xenos::ColorRenderTargetFormat::k_32_FLOAT));
      if (!full_edram_render_target) {
        return;
      }
      assert_false(full_edram_render_target->key().is_depth);
      assert_false(full_edram_render_target->key().Is64bpp());
      uint32_t pitch_tiles = full_edram_render_target->key().pitch_tiles_at_32bpp;
      uint32_t tile_rows = xenos::kEdramTileCount / pitch_tiles;
      assert_true(pitch_tiles * tile_rows == xenos::kEdramTileCount);
      uint32_t row_pitch_samples = pitch_tiles * xenos::kEdramTileWidthSamples;
      VkDeviceSize row_pitch_bytes = VkDeviceSize(row_pitch_samples) * sizeof(uint32_t);
      const uint8_t* snapshot_sample_row = reinterpret_cast<const uint8_t*>(snapshot);
      for (uint32_t y_tile = 0; y_tile < tile_rows; ++y_tile) {
        uint8_t* upload_buffer_tile_row_origin =
            upload_buffer_mapping + row_pitch_bytes * xenos::kEdramTileHeightSamples * y_tile;
        for (uint32_t x_tile = 0; x_tile < pitch_tiles; ++x_tile) {
          uint8_t* upload_buffer_sample_row =
              upload_buffer_tile_row_origin +
              sizeof(uint32_t) * xenos::kEdramTileWidthSamples * x_tile;
          for (uint32_t sample_row = 0; sample_row < xenos::kEdramTileHeightSamples; ++sample_row) {
            std::memcpy(upload_buffer_sample_row, snapshot_sample_row,
                        sizeof(uint32_t) * xenos::kEdramTileWidthSamples);
            snapshot_sample_row += sizeof(uint32_t) * xenos::kEdramTileWidthSamples;
            upload_buffer_sample_row += row_pitch_bytes;
          }
        }
      }
      command_processor_.PushImageMemoryBarrier(
          full_edram_render_target->image(),
          ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT),
          full_edram_render_target->current_stage_mask(), VK_PIPELINE_STAGE_TRANSFER_BIT,
          full_edram_render_target->current_access_mask(), VK_ACCESS_TRANSFER_WRITE_BIT,
          full_edram_render_target->current_layout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      full_edram_render_target->SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_ACCESS_TRANSFER_WRITE_BIT,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      command_processor_.SubmitBarriers(true);
      VkBufferImageCopy copy_region = {};
      copy_region.bufferOffset = upload_buffer_offset;
      copy_region.bufferRowLength = row_pitch_samples;
      copy_region.bufferImageHeight = xenos::kEdramTileHeightSamples * tile_rows;
      copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copy_region.imageSubresource.mipLevel = 0;
      copy_region.imageSubresource.baseArrayLayer = 0;
      copy_region.imageSubresource.layerCount = 1;
      copy_region.imageOffset.x = 0;
      copy_region.imageOffset.y = 0;
      copy_region.imageOffset.z = 0;
      copy_region.imageExtent.width = row_pitch_samples;
      copy_region.imageExtent.height = xenos::kEdramTileHeightSamples * tile_rows;
      copy_region.imageExtent.depth = 1;
      command_buffer.CmdVkCopyBufferToImage(upload_buffer, full_edram_render_target->image(),
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
    } break;

    case Path::kPixelShaderInterlock: {
      std::memcpy(upload_buffer_mapping, snapshot, xenos::kEdramSizeBytes);
      UseEdramBuffer(EdramBufferUsage::kTransferWrite);
      command_processor_.SubmitBarriers(true);
      VkBufferCopy copy_region = {};
      copy_region.srcOffset = upload_buffer_offset;
      copy_region.dstOffset = 0;
      copy_region.size = xenos::kEdramSizeBytes;
      command_buffer.CmdVkCopyBuffer(upload_buffer, edram_buffer_, 1, &copy_region);
    } break;

    default:
      assert_unhandled_case(GetPath());
  }
}

bool VulkanRenderTargetCache::Resolve(const memory::Memory& memory,
                                      VulkanSharedMemory& shared_memory,
                                      VulkanTextureCache& texture_cache,
                                      uint32_t& written_address_out, uint32_t& written_length_out) {
  written_address_out = 0;
  written_length_out = 0;

  // A held back resolve is done first, as this one may use the same render
  // target or texture.
  FlushPendingCopyFreeResolve();
  // The resolve reads the render targets.
  FlushDeferredTransfers();

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  draw_util::ResolveInfo resolve_info;
  if (!draw_util::GetResolveInfo(register_file(), memory, trace_writer_, draw_resolution_scale_x(),
                                 draw_resolution_scale_y(), IsFixedRG16TruncatedToMinus1To1(),
                                 IsFixedRGBA16TruncatedToMinus1To1(), resolve_info)) {
    return false;
  }
  // From an original resolution render target: resolved at the guest
  // resolution, like without scaling (only natively - the EDRAM buffer has the
  // scaled layout).
  bool source_original_resolution = false;
  if (draw_resolution_scaled) {
    bool copying_depth = resolve_info.IsCopyingDepth();
    const draw_util::ResolveEdramInfo& source_edram_info =
        copying_depth ? resolve_info.depth_edram_info : resolve_info.color_edram_info;
    if (IsOriginalResolutionRenderTarget(
            copying_depth ? resolve_info.depth_original_base : resolve_info.color_original_base,
            source_edram_info.pitch_tiles, copying_depth)) {
      source_original_resolution = true;
      draw_resolution_scaled = false;
      if (!draw_util::GetResolveInfo(register_file(), memory, trace_writer_, 1, 1,
                                     IsFixedRG16TruncatedToMinus1To1(),
                                     IsFixedRGBA16TruncatedToMinus1To1(), resolve_info)) {
        return false;
      }
    }
  }

  if (RtDebugLogActive()) {
    const draw_util::ResolveEdramInfo& src =
        resolve_info.IsCopyingDepth() ? resolve_info.depth_edram_info : resolve_info.color_edram_info;
    REXGPU_INFO(
        "[rt-debug] resolve src_select={} src(base={} orig={} pitch={} msaa={} {}) rect "
        "off=({},{}) {}x{} -> dest {:08X} extent {:08X}+{} fmt={} endian={} swap={} bias={} "
        "pitch={} height={} sample={} clear(color={} depth={})",
        uint32_t(resolve_info.rb_copy_control.copy_src_select), uint32_t(src.base_tiles),
        resolve_info.IsCopyingDepth() ? resolve_info.depth_original_base
                                      : resolve_info.color_original_base,
        uint32_t(src.pitch_tiles), 1u << uint32_t(src.msaa_samples),
        RtFormatName(src.is_depth, src.format),
        uint32_t(resolve_info.coordinate_info.edram_offset_x_div_8) * 8,
        uint32_t(resolve_info.coordinate_info.edram_offset_y_div_8) * 8,
        uint32_t(resolve_info.coordinate_info.width_div_8) * 8, resolve_info.height_div_8 * 8,
        resolve_info.copy_dest_base, resolve_info.copy_dest_extent_start,
        resolve_info.copy_dest_extent_length, uint32_t(resolve_info.copy_dest_info.copy_dest_format),
        uint32_t(resolve_info.copy_dest_info.copy_dest_endian),
        uint32_t(resolve_info.copy_dest_info.copy_dest_swap),
        int32_t(resolve_info.copy_dest_info.copy_dest_exp_bias),
        uint32_t(resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32) * 32,
        uint32_t(resolve_info.copy_dest_coordinate_info.height_aligned_div_32) * 32,
        uint32_t(resolve_info.copy_dest_coordinate_info.copy_sample_select),
        uint32_t(resolve_info.rb_copy_control.color_clear_enable),
        uint32_t(resolve_info.rb_copy_control.depth_clear_enable));
  }

  // Nothing to copy/clear.
  if (!resolve_info.coordinate_info.width_div_8 || !resolve_info.height_div_8) {
    return true;
  }

  if (command_processor_.gpu_profiler().per_draw()) {
    // Key: copying depth (bit 60), destination format and base; rectangle
    // size, color clear (bit 1) and depth clear (bit 0).
    command_processor_.gpu_profiler().MarkKeyed(
        command_processor_.deferred_command_buffer(), VulkanGpuProfiler::Category::kResolve,
        (uint64_t(resolve_info.IsCopyingDepth()) << 60) |
            (uint64_t(resolve_info.copy_dest_info.copy_dest_format) << 40) |
            resolve_info.copy_dest_base,
        (uint64_t(resolve_info.coordinate_info.width_div_8) * 8 << 48) |
            (uint64_t(resolve_info.height_div_8) * 8 << 32) |
            (uint32_t(resolve_info.rb_copy_control.color_clear_enable) << 1) |
            uint32_t(resolve_info.rb_copy_control.depth_clear_enable));
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();

  // Copying.
  bool copied = false;
  // Resolved data still only in textures, in the range about to be written.
  if (resolve_info.copy_dest_extent_length && !pending_scaled_resolve_memory_.empty()) {
    PrepareScaledResolveMemoryForResolve(resolve_info.copy_dest_extent_start,
                                         resolve_info.copy_dest_extent_length);
  }
  // Textures sampling the destination must be found while their data still
  // matches memory, before the resolved range is invalidated.
  NativeResolvePlan native_resolve_plan;
  bool native_resolve_planned =
      native_resolve_enabled_ &&
      PrepareNativeResolve(resolve_info, texture_cache, native_resolve_plan);
  if (!native_resolve_planned && native_resolve_enabled_ && resolve_info.copy_dest_extent_length) {
    // Reports resolves still done by dumping the EDRAM and running the
    // resolve shader (each distinct kind once).
    static std::unordered_set<uint64_t> fallback_resolves_logged;
    const draw_util::ResolveEdramInfo& source_info = resolve_info.IsCopyingDepth()
                                                         ? resolve_info.depth_edram_info
                                                         : resolve_info.color_edram_info;
    uint64_t fallback_key =
        (uint64_t(resolve_info.IsCopyingDepth()) << 63) |
        (uint64_t(source_info.msaa_samples) << 56) | (uint64_t(source_info.format) << 48) |
        (uint64_t(resolve_info.copy_dest_info.copy_dest_format) << 40) |
        (uint64_t(uint8_t(int8_t(resolve_info.copy_dest_info.copy_dest_exp_bias))) << 32) |
        resolve_info.copy_dest_base;
    if (fallback_resolves_logged.size() < 64 &&
        fallback_resolves_logged.insert(fallback_key).second) {
      REXGPU_INFO(
          "[native-fallback] {} resolve through the EDRAM emulation: {}x MSAA source format {} "
          "-> destination format {}, exponent bias {}, {}x{} at {:08X}",
          resolve_info.IsCopyingDepth() ? "depth" : "color",
          1u << uint32_t(source_info.msaa_samples), uint32_t(source_info.format),
          uint32_t(resolve_info.copy_dest_info.copy_dest_format),
          int32_t(resolve_info.copy_dest_info.copy_dest_exp_bias),
          uint32_t(resolve_info.coordinate_info.width_div_8) * 8, resolve_info.height_div_8 * 8,
          resolve_info.copy_dest_base);
    }
  }
  // The first target's draw can also write the resolved memory, replacing the
  // EDRAM dump and the resolve dispatch.
  bool native_resolve_single_pass =
      native_resolve_planned && native_resolve_plan.can_write_memory &&
      REXCVAR_GET(native_resolve_single_pass) &&
      GetNativeResolvePipeline(native_resolve_plan.shader,
                               native_resolve_plan.targets[0].format) != VK_NULL_HANDLE;
  // Compact outputs fit in the first descriptor, including on devices where
  // the scaled resolve cannot stand in for the whole shared-memory array.
  bool native_buffer_candidate =
      native_resolve_planned && REXCVAR_GET(native_resolve_buffers) &&
      REXCVAR_GET(native_resolve_single_pass) &&
      native_resolve_plan.targets[0].width >= native_resolve_plan.x1 &&
      native_resolve_plan.targets[0].height >= native_resolve_plan.y1 &&
      GetNativeResolvePipeline(native_resolve_plan.shader, native_resolve_plan.targets[0].format) !=
          VK_NULL_HANDLE;
  if (native_buffer_candidate && draw_resolution_scaled && !native_resolve_plan.memory_only &&
      REXCVAR_GET(native_resolve_buffer_texture_first) &&
      REXCVAR_GET(native_resolve_scaled_lazy_memory) &&
      (native_resolve_plan.memory_flags & kNativeResolveFlagMemoryScaled)) {
    // The texture already owns the result. Packing a second full-resolution
    // copy here costs bandwidth even when nothing needs the raw bytes.
    native_buffer_candidate = false;
  }
  if (resolve_info.copy_dest_extent_length && native_buffer_candidate &&
      TryNativeResolveBuffer(resolve_info, native_resolve_plan, shared_memory, texture_cache,
                             source_original_resolution)) {
    written_address_out = resolve_info.copy_dest_extent_start;
    written_length_out = resolve_info.copy_dest_extent_length;
    copied = true;
  } else if (resolve_info.copy_dest_extent_length && native_resolve_single_pass &&
             draw_resolution_scaled) {
    // The first target's draw writes the scaled resolve buffer, bound from the
    // destination's base (the address the scaled resolve shaders use).
    uint32_t bytes_per_block_log2 =
        (native_resolve_plan.memory_flags & kNativeResolveFlagMemory64bpp) ? 3 : 2;
    uint32_t dest_range_unscaled = resolve_info.copy_dest_extent_start +
                                   resolve_info.copy_dest_extent_length -
                                   native_resolve_plan.dest_base;
    uint64_t dest_base_scaled, dest_range_scaled, dest_use_start_scaled, dest_use_length_scaled;
    VkDescriptorSet scaled_memory_descriptor_set = VK_NULL_HANDLE;
    if (resolve_info.copy_dest_extent_start >= native_resolve_plan.dest_base &&
        texture_cache.GetScaledResolveRange(native_resolve_plan.dest_base, dest_range_unscaled,
                                            bytes_per_block_log2, dest_base_scaled,
                                            dest_range_scaled) &&
        texture_cache.GetScaledResolveRange(
            resolve_info.copy_dest_extent_start, resolve_info.copy_dest_extent_length,
            bytes_per_block_log2, dest_use_start_scaled, dest_use_length_scaled) &&
        texture_cache.CommitScaledResolveRange(native_resolve_plan.dest_base, dest_range_unscaled,
                                               bytes_per_block_log2)) {
      scaled_memory_descriptor_set = command_processor_.AllocateSingleTransientDescriptor(
          VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferGuestShaders);
    }
    // The memory is written from the first target only if something reads it
    // before it's replaced. Color targets hold the raw bits the memory would;
    // depth targets hold converted depth (invertible) but no stencil, which is
    // captured separately.
    bool lazy_memory =
        REXCVAR_GET(native_resolve_scaled_lazy_memory) && !native_resolve_plan.memory_only;
    uint32_t stencil_capture = UINT32_MAX;
    bool stencil_capture_quads = false;
    VkDescriptorSet stencil_capture_descriptor_set = VK_NULL_HANDLE;
    uint32_t verify_stencil_capture = UINT32_MAX;
    bool stencil_uniform = false;
    uint32_t stencil_uniform_value = 0;
    if (lazy_memory && IsNativeResolveDepthShader(native_resolve_plan.shader)) {
      // Copying the stencil to a buffer needs a single-sampled image.
      lazy_memory = REXCVAR_GET(native_resolve_scaled_lazy_depth) &&
                    native_resolve_plan.source->key().msaa_samples == xenos::MsaaSamples::k1X &&
                    EnsureScaledMemoryWritebackDepthPipeline();
      if (lazy_memory) {
        uint32_t host_x0 = native_resolve_plan.x0 * draw_resolution_scale_x();
        uint32_t host_y0 = native_resolve_plan.y0 * draw_resolution_scale_y();
        uint32_t host_width =
            (native_resolve_plan.x1 - native_resolve_plan.x0) * draw_resolution_scale_x();
        uint32_t host_height =
            (native_resolve_plan.y1 - native_resolve_plan.y0) * draw_resolution_scale_y();
        VkDeviceSize capture_size = VkDeviceSize(host_width) * host_height;
        stencil_uniform = GetUniformStencil(*native_resolve_plan.source, native_resolve_plan.x0,
                                            native_resolve_plan.y0, native_resolve_plan.x1,
                                            native_resolve_plan.y1, stencil_uniform_value);
        if (stencil_uniform) {
          // The write-back stores the known stencil. With verification, a copied
          // capture to compare it with.
          if (REXCVAR_GET(native_resolve_debug_verify_stencil_capture)) {
            verify_stencil_capture = AcquireScaledStencilCapture(capture_size);
          }
        } else {
          stencil_capture = AcquireScaledStencilCapture(capture_size);
          lazy_memory = stencil_capture != UINT32_MAX;
        }
        // The resolve draw captures whole 2x2 quads.
        stencil_capture_quads = stencil_capture != UINT32_MAX &&
                                native_resolve_quad_stencil_capture_ &&
                                !((host_x0 | host_y0 | host_width | host_height) & 1);
        if (stencil_capture_quads) {
          stencil_capture_descriptor_set = command_processor_.AllocateSingleTransientDescriptor(
              VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferGuestShaders);
          if (stencil_capture_descriptor_set != VK_NULL_HANDLE) {
            VkDescriptorBufferInfo capture_buffer_info;
            capture_buffer_info.buffer = scaled_stencil_captures_[stencil_capture].buffer;
            capture_buffer_info.offset = 0;
            capture_buffer_info.range = VK_WHOLE_SIZE;
            VkWriteDescriptorSet capture_write;
            capture_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            capture_write.pNext = nullptr;
            capture_write.dstSet = stencil_capture_descriptor_set;
            capture_write.dstBinding = 0;
            capture_write.dstArrayElement = 0;
            capture_write.descriptorCount = 1;
            capture_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            capture_write.pImageInfo = nullptr;
            capture_write.pBufferInfo = &capture_buffer_info;
            capture_write.pTexelBufferView = nullptr;
            dfn.vkUpdateDescriptorSets(device, 1, &capture_write, 0, nullptr);
            // Earlier write-backs may still read it, earlier captures write it.
            command_processor_.PushBufferMemoryBarrier(
                capture_buffer_info.buffer, 0, VK_WHOLE_SIZE,
                VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                    VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                false);
          } else {
            stencil_capture_quads = false;
          }
        }
        if (stencil_capture_quads && REXCVAR_GET(native_resolve_debug_verify_stencil_capture)) {
          verify_stencil_capture = AcquireScaledStencilCapture(capture_size);
        }
      }
    }
    if (lazy_memory) {
      texture_cache.MarkRangeAsResolved(resolve_info.copy_dest_extent_start,
                                        resolve_info.copy_dest_extent_length);
      written_address_out = resolve_info.copy_dest_extent_start;
      written_length_out = resolve_info.copy_dest_extent_length;
      if (stencil_capture_descriptor_set == VK_NULL_HANDLE &&
          CanResolveCopyFree(native_resolve_plan, texture_cache)) {
        // Held back until the next draw. The texture counts as written: nothing
        // uses it before the copy or the exchange.
        pending_copy_free_resolve_.active = true;
        pending_copy_free_resolve_.plan = native_resolve_plan;
        pending_copy_free_resolve_.texture_cache = &texture_cache;
        texture_cache.EndNativeResolveWrite(native_resolve_plan.targets[0]);
      } else {
        PerformNativeResolve(texture_cache, native_resolve_plan, false, VK_NULL_HANDLE,
                             stencil_capture_descriptor_set);
      }
      if (stencil_capture_quads) {
        command_processor_.PushBufferMemoryBarrier(
            scaled_stencil_captures_[stencil_capture].buffer, 0, VK_WHOLE_SIZE,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED, false);
      } else if (stencil_capture != UINT32_MAX) {
        CaptureScaledResolveStencil(*native_resolve_plan.source, native_resolve_plan.x0,
                                    native_resolve_plan.y0, native_resolve_plan.x1,
                                    native_resolve_plan.y1, stencil_capture);
      }
      if (verify_stencil_capture != UINT32_MAX) {
        CaptureScaledResolveStencil(*native_resolve_plan.source, native_resolve_plan.x0,
                                    native_resolve_plan.y0, native_resolve_plan.x1,
                                    native_resolve_plan.y1, verify_stencil_capture);
      }
      PendingScaledResolveMemory& pending = pending_scaled_resolve_memory_.emplace_back();
      pending.is_depth = IsNativeResolveDepthShader(native_resolve_plan.shader);
      pending.depth_float_view = native_resolve_plan.shader == NativeResolveShader::kDepthFloat;
      pending.flags = native_resolve_plan.flags;
      pending.stencil_capture = stencil_capture;
      pending.stencil_capture_quads = stencil_capture_quads;
      pending.stencil_uniform = stencil_uniform;
      pending.stencil_uniform_value = stencil_uniform_value;
      pending.verify_stencil_capture = verify_stencil_capture;
      pending.texture = native_resolve_plan.targets[0].texture;
      pending.dest_base = native_resolve_plan.dest_base;
      pending.extent_start = resolve_info.copy_dest_extent_start;
      pending.extent_length = resolve_info.copy_dest_extent_length;
      pending.dest_pitch_texels = native_resolve_plan.dest_pitch_texels;
      pending.x0 = native_resolve_plan.x0;
      pending.y0 = native_resolve_plan.y0;
      pending.x1 = native_resolve_plan.x1;
      pending.y1 = native_resolve_plan.y1;
      pending.memory_flags = native_resolve_plan.memory_flags;
      if (native_resolve_plan.shader == NativeResolveShader::kColorUnorm) {
        pending.unorm_packing = uint32_t(native_resolve_plan.packing);
      }
      texture_cache.AddScaledMemoryPending(pending.texture, 1);
      pending_scaled_resolve_texture_cache_ = &texture_cache;
      copied = true;
    } else if (REXCVAR_GET(native_resolve_debug_skip_scaled_memory) &&
               !native_resolve_plan.memory_only) {
      texture_cache.MarkRangeAsResolved(resolve_info.copy_dest_extent_start,
                                        resolve_info.copy_dest_extent_length);
      written_address_out = resolve_info.copy_dest_extent_start;
      written_length_out = resolve_info.copy_dest_extent_length;
      PerformNativeResolve(texture_cache, native_resolve_plan, false);
      copied = true;
    } else if (scaled_memory_descriptor_set != VK_NULL_HANDLE) {
      VkDescriptorBufferInfo scaled_memory_buffer_info;
      scaled_memory_buffer_info.buffer = texture_cache.scaled_resolve_buffer();
      scaled_memory_buffer_info.offset = dest_base_scaled;
      scaled_memory_buffer_info.range = dest_range_scaled;
      VkWriteDescriptorSet scaled_memory_write;
      scaled_memory_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      scaled_memory_write.pNext = nullptr;
      scaled_memory_write.dstSet = scaled_memory_descriptor_set;
      scaled_memory_write.dstBinding = 0;
      scaled_memory_write.dstArrayElement = 0;
      scaled_memory_write.descriptorCount = 1;
      scaled_memory_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      scaled_memory_write.pImageInfo = nullptr;
      scaled_memory_write.pBufferInfo = &scaled_memory_buffer_info;
      scaled_memory_write.pTexelBufferView = nullptr;
      dfn.vkUpdateDescriptorSets(device, 1, &scaled_memory_write, 0, nullptr);
      texture_cache.UseScaledResolveBufferForWrite(dest_use_start_scaled, dest_use_length_scaled);
      texture_cache.MarkRangeAsResolved(resolve_info.copy_dest_extent_start,
                                        resolve_info.copy_dest_extent_length);
      written_address_out = resolve_info.copy_dest_extent_start;
      written_length_out = resolve_info.copy_dest_extent_length;
      PerformNativeResolve(texture_cache, native_resolve_plan, true, scaled_memory_descriptor_set);
      copied = true;
    } else {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to obtain the scaled resolve destination "
          "memory region");
    }
  } else if (resolve_info.copy_dest_extent_length && native_resolve_single_pass) {
    if (shared_memory.RequestRange(resolve_info.copy_dest_extent_start,
                                   resolve_info.copy_dest_extent_length)) {
      texture_cache.MarkRangeAsResolved(resolve_info.copy_dest_extent_start,
                                        resolve_info.copy_dest_extent_length,
                                        source_original_resolution);
      written_address_out = resolve_info.copy_dest_extent_start;
      written_length_out = resolve_info.copy_dest_extent_length;
      shared_memory.Use(VulkanSharedMemory::Usage::kGuestDrawReadWrite,
                        std::make_pair(resolve_info.copy_dest_extent_start,
                                       resolve_info.copy_dest_extent_length));
      PerformNativeResolve(texture_cache, native_resolve_plan, true);
      copied = true;
    } else {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to obtain the resolve destination "
          "memory region");
    }
  } else if (resolve_info.copy_dest_extent_length && source_original_resolution) {
    static bool original_legacy_logged = false;
    if (!original_legacy_logged) {
      original_legacy_logged = true;
      REXGPU_WARN(
          "Resolve from an original resolution render target that can't be done natively - "
          "skipped");
    }
    copied = true;
  } else if (resolve_info.copy_dest_extent_length) {
    draw_util::ResolveCopyShaderConstants copy_shader_constants;
    uint32_t copy_group_count_x, copy_group_count_y;
    draw_util::ResolveCopyShaderIndex copy_shader =
        resolve_info.GetCopyShader(draw_resolution_scale_x(), draw_resolution_scale_y(),
                                   copy_shader_constants, copy_group_count_x, copy_group_count_y);
    assert_true(copy_group_count_x && copy_group_count_y);
    if (copy_shader != draw_util::ResolveCopyShaderIndex::kUnknown) {
      const draw_util::ResolveCopyShaderInfo& copy_shader_info =
          draw_util::resolve_copy_shader_info[size_t(copy_shader)];
      bool direct_resolved = false;
      if (GetPath() == Path::kHostRenderTargets) {
        if (REXCVAR_GET(direct_host_resolve)) {
          direct_resolved =
              TryResolveCopyDirectly(resolve_info, copy_shader, draw_resolution_scaled);
          if (direct_resolved) {
            ++direct_resolve_success_count_;
          } else {
            ++direct_resolve_fallback_count_;
          }
        }
        if (!direct_resolved) {
          // Dump the current contents of the render targets owning the affected
          // range to edram_buffer_.
          uint32_t dump_base;
          uint32_t dump_row_length_used;
          uint32_t dump_rows;
          uint32_t dump_pitch;
          resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
          if (!DumpRenderTargets(dump_base, dump_row_length_used, dump_rows, dump_pitch)) {
            REXGPU_ERROR("VulkanRenderTargetCache: Failed to dump host render targets for resolve");
            return false;
          }
        }
      }

      uint32_t copy_dest_range_unscaled = resolve_info.copy_dest_extent_start -
                                          resolve_info.copy_dest_base +
                                          resolve_info.copy_dest_extent_length;
      uint64_t copy_dest_base = resolve_info.copy_dest_base;
      uint64_t copy_dest_range_length = copy_dest_range_unscaled;
      uint64_t copy_dest_use_start = resolve_info.copy_dest_extent_start;
      uint64_t copy_dest_use_length = resolve_info.copy_dest_extent_length;
      if (draw_resolution_scaled) {
        if (!texture_cache.GetScaledResolveRange(
                resolve_info.copy_dest_base, copy_dest_range_unscaled,
                copy_shader_info.dest_bpe_log2, copy_dest_base, copy_dest_range_length) ||
            !texture_cache.GetScaledResolveRange(
                resolve_info.copy_dest_extent_start, resolve_info.copy_dest_extent_length,
                copy_shader_info.dest_bpe_log2, copy_dest_use_start, copy_dest_use_length)) {
          REXGPU_ERROR(
              "VulkanRenderTargetCache: Failed to map scaled resolve "
              "destination range (base={:08X}, length={:08X})",
              resolve_info.copy_dest_base, copy_dest_range_unscaled);
          return false;
        }
      }

      // Make sure there is memory to write to.
      bool copy_dest_committed;
      if (draw_resolution_scaled) {
        copy_dest_committed = texture_cache.CommitScaledResolveRange(
            resolve_info.copy_dest_base, copy_dest_range_unscaled, copy_shader_info.dest_bpe_log2);
      } else {
        copy_dest_committed = shared_memory.RequestRange(resolve_info.copy_dest_extent_start,
                                                         resolve_info.copy_dest_extent_length);
      }
      if (!copy_dest_committed) {
        REXGPU_ERROR(
            "VulkanRenderTargetCache: Failed to obtain the resolve destination "
            "memory region");
      } else {
        // TODO(Triang3l): Switching between descriptors if exceeding
        // maxStorageBufferRange.
        // TODO(Triang3l): Use a single 512 MB shared memory binding if
        // possible.
        VkDescriptorSet descriptor_set_dest = command_processor_.AllocateSingleTransientDescriptor(
            VulkanCommandProcessor::SingleTransientDescriptorLayout ::kStorageBufferCompute);
        if (descriptor_set_dest != VK_NULL_HANDLE) {
          // Write the destination descriptor.
          VkDescriptorBufferInfo write_descriptor_set_dest_buffer_info;
          write_descriptor_set_dest_buffer_info.buffer = draw_resolution_scaled
                                                             ? texture_cache.scaled_resolve_buffer()
                                                             : shared_memory.buffer();
          write_descriptor_set_dest_buffer_info.offset = copy_dest_base;
          write_descriptor_set_dest_buffer_info.range = copy_dest_range_length;
          VkWriteDescriptorSet write_descriptor_set_dest;
          write_descriptor_set_dest.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          write_descriptor_set_dest.pNext = nullptr;
          write_descriptor_set_dest.dstSet = descriptor_set_dest;
          write_descriptor_set_dest.dstBinding = 0;
          write_descriptor_set_dest.dstArrayElement = 0;
          write_descriptor_set_dest.descriptorCount = 1;
          write_descriptor_set_dest.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          write_descriptor_set_dest.pImageInfo = nullptr;
          write_descriptor_set_dest.pBufferInfo = &write_descriptor_set_dest_buffer_info;
          write_descriptor_set_dest.pTexelBufferView = nullptr;
          dfn.vkUpdateDescriptorSets(device, 1, &write_descriptor_set_dest, 0, nullptr);

          // Submit the resolve.
          if (draw_resolution_scaled) {
            texture_cache.UseScaledResolveBufferForWrite(copy_dest_use_start, copy_dest_use_length);
          } else {
            shared_memory.Use(VulkanSharedMemory::Usage::kComputeWrite,
                              std::pair<uint32_t, uint32_t>(uint32_t(copy_dest_use_start),
                                                            uint32_t(copy_dest_use_length)));
          }
          UseEdramBuffer(EdramBufferUsage::kComputeRead);
          command_processor_.BindExternalComputePipeline(
              resolve_copy_pipelines_[size_t(copy_shader)]);
          VkDescriptorSet descriptor_sets[kResolveCopyDescriptorSetCount] = {};
          descriptor_sets[kResolveCopyDescriptorSetEdram] = edram_storage_buffer_descriptor_set_;
          descriptor_sets[kResolveCopyDescriptorSetDest] = descriptor_set_dest;
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_COMPUTE, resolve_copy_pipeline_layout_, 0,
              uint32_t(rex::countof(descriptor_sets)), descriptor_sets, 0, nullptr);
          if (draw_resolution_scaled) {
            command_buffer.CmdVkPushConstants(
                resolve_copy_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(copy_shader_constants.dest_relative), &copy_shader_constants.dest_relative);
          } else {
            // TODO(Triang3l): Proper dest_base in case of one 512 MB shared
            // memory binding, or multiple shared memory bindings in case of
            // splitting due to maxStorageBufferRange overflow.
            copy_shader_constants.dest_base -=
                uint32_t(write_descriptor_set_dest_buffer_info.offset);
            command_buffer.CmdVkPushConstants(
                resolve_copy_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(copy_shader_constants), &copy_shader_constants);
          }
          command_processor_.SubmitBarriers(true);
          command_buffer.CmdVkDispatch(copy_group_count_x, copy_group_count_y, 1);

          // Invalidate textures and mark the range as scaled if needed.
          texture_cache.MarkRangeAsResolved(resolve_info.copy_dest_extent_start,
                                            resolve_info.copy_dest_extent_length);
          written_address_out = resolve_info.copy_dest_extent_start;
          written_length_out = resolve_info.copy_dest_extent_length;
          copied = true;
          if (native_resolve_planned) {
            if (CanResolveCopyFree(native_resolve_plan, texture_cache)) {
              // Held back until the next draw, like with lazy scaled memory.
              pending_copy_free_resolve_.active = true;
              pending_copy_free_resolve_.plan = native_resolve_plan;
              pending_copy_free_resolve_.texture_cache = &texture_cache;
              texture_cache.EndNativeResolveWrite(native_resolve_plan.targets[0]);
            } else {
              PerformNativeResolve(texture_cache, native_resolve_plan, false);
            }
          }
        }
      }
    }
  } else {
    copied = true;
  }

  // Clearing.
  bool cleared = false;
  bool clear_depth = resolve_info.IsClearingDepth();
  bool clear_color = resolve_info.IsClearingColor();
  if (clear_depth && (resolve_info.rb_depth_clear & 0xFF)) {
    g_stencil_nonzero_clears.fetch_add(1, std::memory_order_relaxed);
  }
  if (clear_depth || clear_color) {
    switch (GetPath()) {
      case Path::kHostRenderTargets: {
        Transfer::Rectangle clear_rectangle;
        RenderTarget* clear_render_targets[2];
        // If PrepareHostRenderTargetsResolveClear returns false, may be just an
        // empty region (success) or an error - don't care.
        if (PrepareHostRenderTargetsResolveClear(resolve_info, clear_rectangle,
                                                 clear_render_targets[0], clear_transfers_[0],
                                                 clear_render_targets[1], clear_transfers_[1])) {
          uint64_t clear_values[2];
          clear_values[0] = resolve_info.rb_depth_clear;
          clear_values[1] =
              resolve_info.rb_color_clear | (uint64_t(resolve_info.rb_color_clear_lo) << 32);
          PerformTransfersAndResolveClears(2, clear_render_targets, clear_transfers_, clear_values,
                                           &clear_rectangle);
        }
        cleared = true;
      } break;
      case Path::kPixelShaderInterlock: {
        UseEdramBuffer(EdramBufferUsage::kComputeWrite);
        // Should be safe to only commit once (if was accessed as unordered or
        // with fragment shader interlock previously - if there was nothing to
        // copy, only to clear, for some reason, for instance), overlap of the
        // depth and the color ranges is highly unlikely.
        CommitEdramBufferShaderWrites();
        command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                               resolve_fsi_clear_pipeline_layout_, 0, 1,
                                               &edram_storage_buffer_descriptor_set_, 0, nullptr);
        std::pair<uint32_t, uint32_t> clear_group_count = resolve_info.GetClearShaderGroupCount(
            draw_resolution_scale_x(), draw_resolution_scale_y());
        assert_true(clear_group_count.first && clear_group_count.second);
        if (clear_depth) {
          command_processor_.BindExternalComputePipeline(resolve_fsi_clear_32bpp_pipeline_);
          draw_util::ResolveClearShaderConstants depth_clear_constants;
          resolve_info.GetDepthClearShaderConstants(depth_clear_constants);
          command_buffer.CmdVkPushConstants(resolve_fsi_clear_pipeline_layout_,
                                            VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                            sizeof(depth_clear_constants), &depth_clear_constants);
          command_processor_.SubmitBarriers(true);
          command_buffer.CmdVkDispatch(clear_group_count.first, clear_group_count.second, 1);
        }
        if (clear_color) {
          command_processor_.BindExternalComputePipeline(
              resolve_info.color_edram_info.format_is_64bpp ? resolve_fsi_clear_64bpp_pipeline_
                                                            : resolve_fsi_clear_32bpp_pipeline_);
          draw_util::ResolveClearShaderConstants color_clear_constants;
          resolve_info.GetColorClearShaderConstants(color_clear_constants);
          if (clear_depth) {
            // Non-RT-specific constants have already been set.
            command_buffer.CmdVkPushConstants(
                resolve_fsi_clear_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                uint32_t(offsetof(draw_util::ResolveClearShaderConstants, rt_specific)),
                sizeof(color_clear_constants.rt_specific), &color_clear_constants.rt_specific);
          } else {
            command_buffer.CmdVkPushConstants(
                resolve_fsi_clear_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(color_clear_constants), &color_clear_constants);
          }
          command_processor_.SubmitBarriers(true);
          command_buffer.CmdVkDispatch(clear_group_count.first, clear_group_count.second, 1);
        }
        MarkEdramBufferModified();
        cleared = true;
      } break;
      default:
        assert_unhandled_case(GetPath());
    }
  } else {
    cleared = true;
  }

  return copied && cleared;
}

bool VulkanRenderTargetCache::Update(bool is_rasterization_done,
                                     reg::RB_DEPTHCONTROL normalized_depth_control,
                                     uint32_t normalized_color_mask, const Shader& vertex_shader) {
  if (!RenderTargetCache::Update(is_rasterization_done, normalized_depth_control,
                                 normalized_color_mask, vertex_shader)) {
    return false;
  }

  auto rb_surface_info = register_file().Get<reg::RB_SURFACE_INFO>();

  RenderPassKey render_pass_key;
  // Needed even with the fragment shader interlock render backend for passing
  // the sample count to the pipeline cache.
  render_pass_key.msaa_samples = GetKeyMsaaSamples(rb_surface_info.msaa_samples);

  switch (GetPath()) {
    case Path::kHostRenderTargets: {
      RenderTarget* const* depth_and_color_render_targets =
          last_update_accumulated_render_targets();

      const std::vector<Transfer>* transfers = last_update_transfers();
      bool any_transfers = false;
      for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets && !any_transfers; ++i) {
        any_transfers = !transfers[i].empty();
      }
      if (RtDebugLogActive() && any_transfers) {
        std::string bindings;
        for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
          RenderTarget* render_target = depth_and_color_render_targets[i];
          if (!render_target) {
            continue;
          }
          RenderTargetKey key = render_target->key();
          bindings += fmt::format(" {}(base={} pitch={} msaa={} {})", i ? "color" : "depth",
                                  uint32_t(key.base_tiles), uint32_t(key.pitch_tiles_at_32bpp),
                                  1u << uint32_t(key.msaa_samples),
                                  RtFormatName(key.is_depth, key.resource_format));
        }
        REXGPU_INFO("[rt-debug] bind with transfers:{}", bindings);
      }
      static const bool draw_log = std::getenv("REX_RT_DRAW_LOG") != nullptr;
      if (draw_log && RtDebugLogActive()) {
        const RegisterFile& regs = register_file();
        auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
        const Shader* pixel_shader = command_processor_.active_pixel_shader();
        std::string bindings;
        for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
          RenderTarget* render_target = depth_and_color_render_targets[i];
          if (!render_target) {
            continue;
          }
          RenderTargetKey key = render_target->key();
          bindings += fmt::format(" {}{}:{}/{}", i ? "c" : "d", i ? i - 1 : 0,
                                  uint32_t(key.base_tiles), uint32_t(key.pitch_tiles_at_32bpp));
        }
        Transfer::Rectangle rectangle = {};
        bool exact_edges = false;
        uint32_t overwritten = GetDrawOverwrittenRenderTargets(
            normalized_depth_control, normalized_color_mask, vertex_shader, rectangle, &exact_edges);
        REXGPU_INFO(
            "[rt-draw] VS {:016X} PS {:016X} prim {} n {} src {} mask {:04X} blend0 {:08X} "
            "vte {:08X} depth {:08X}{}{} overwrites {:#x} ({},{} {}x{}{}) loop16 {:08X} loop31 "
            "{:08X} "
            "idx {} swap {} offset {} min {} max {:X} vbind {}",
            vertex_shader.ucode_data_hash(), pixel_shader ? pixel_shader->ucode_data_hash() : 0,
            uint32_t(initiator.prim_type), uint32_t(initiator.num_indices),
            uint32_t(initiator.source_select), normalized_color_mask,
            regs[reg::RB_BLENDCONTROL::rt_register_indices[0]], regs[XE_GPU_REG_PA_CL_VTE_CNTL],
            normalized_depth_control.value, bindings, any_transfers ? " XFER" : "", overwritten,
            rectangle.x_pixels, rectangle.y_pixels, rectangle.width_pixels, rectangle.height_pixels,
            exact_edges ? " exact" : "", regs[XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + 16],
            regs[XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + 31],
            initiator.index_size == xenos::IndexFormat::kInt32 ? 32 : 16,
            uint32_t(regs.Get<reg::VGT_DMA_SIZE>().swap_mode),
            regs.Get<reg::VGT_INDX_OFFSET>().indx_offset,
            regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx, regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx,
            vertex_shader.vertex_bindings().size());
      }
      bool skip_overwritten_transfers =
          native_rt_mode_ && REXCVAR_GET(native_rt_skip_overwritten_transfers);
      if (deferred_transfer_targets_) {
        // Still held back if this draw clears all of the render targets the
        // transfers go to, next to or over what was cleared before.
        bool keep_deferring = false;
        if (!any_transfers && skip_overwritten_transfers &&
            !std::memcmp(deferred_transfer_bindings_, depth_and_color_render_targets,
                         sizeof(deferred_transfer_bindings_))) {
          Transfer::Rectangle rectangle;
          bool exact_edges;
          uint32_t overwritten = GetDrawOverwrittenRenderTargets(
              normalized_depth_control, normalized_color_mask, vertex_shader, rectangle,
              &exact_edges);
          keep_deferring = exact_edges &&
                           (overwritten & deferred_transfer_targets_) == deferred_transfer_targets_ &&
                           MergeTransferCutout(deferred_transfer_cutout_, rectangle);
          if (keep_deferring && RtDebugLogActive()) {
            REXGPU_INFO("[rt-debug] transfers still held back, cleared ({},{}) {}x{}",
                        deferred_transfer_cutout_.x_pixels, deferred_transfer_cutout_.y_pixels,
                        deferred_transfer_cutout_.width_pixels,
                        deferred_transfer_cutout_.height_pixels);
          }
        }
        if (!keep_deferring) {
          FlushDeferredTransfers();
        }
      }

      Transfer::Rectangle overwrite_rectangle;
      uint32_t overwritten_render_targets = 0;
      bool overwrite_exact_edges = false;
      if (skip_overwritten_transfers && any_transfers) {
        overwritten_render_targets = GetDrawOverwrittenRenderTargets(
            normalized_depth_control, normalized_color_mask, vertex_shader, overwrite_rectangle,
            &overwrite_exact_edges);
      }
      if (overwritten_render_targets) {
        // What this draw overwrites doesn't need the previous owner's data.
        // Transfers entirely inside the rectangle are dropped, the rest are
        // performed around it.
        std::array<std::vector<Transfer>, 1 + xenos::kMaxColorRenderTargets> kept_transfers;
        std::array<std::vector<Transfer>, 1 + xenos::kMaxColorRenderTargets> cut_transfers;
        RenderTarget* kept_render_targets[1 + xenos::kMaxColorRenderTargets] = {};
        RenderTarget* cut_render_targets[1 + xenos::kMaxColorRenderTargets] = {};
        uint32_t dropped_count = 0;
        uint32_t cut_targets = 0;
        for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
          RenderTarget* render_target = depth_and_color_render_targets[i];
          if (!render_target || transfers[i].empty()) {
            continue;
          }
          if (!(overwritten_render_targets & (uint32_t(1) << i))) {
            kept_render_targets[i] = render_target;
            kept_transfers[i] = transfers[i];
            continue;
          }
          RenderTargetKey key = render_target->key();
          for (const Transfer& transfer : transfers[i]) {
            Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
            if (transfer.GetRectangles(key.base_tiles, key.GetPitchTiles(), key.msaa_samples,
                                       key.Is64bpp(), rectangles, &overwrite_rectangle)) {
              cut_transfers[i].push_back(transfer);
            } else {
              ++dropped_count;
            }
          }
          if (!cut_transfers[i].empty()) {
            cut_render_targets[i] = render_target;
            cut_targets |= uint32_t(1) << i;
          }
        }
        // The rest can wait in case the following draws are clears too. The
        // held back transfers then go only where nothing has been drawn, so the
        // clears must have exact edges, and the transfers' sources mustn't be
        // drawn to meanwhile.
        bool defer = cut_targets && overwrite_exact_edges &&
                     REXCVAR_GET(native_rt_defer_overwritten_transfers);
        for (uint32_t i = 0; defer && i < 1 + xenos::kMaxColorRenderTargets; ++i) {
          for (const Transfer& transfer : cut_transfers[i]) {
            for (uint32_t j = 0; j < 1 + xenos::kMaxColorRenderTargets; ++j) {
              RenderTarget* bound = depth_and_color_render_targets[j];
              if (bound && (transfer.source == bound || transfer.host_depth_source == bound)) {
                defer = false;
              }
            }
          }
        }
        if (RtDebugLogActive()) {
          REXGPU_INFO(
              "[rt-debug] draw overwrites rts {:#x} in ({},{}) {}x{}: {} transfers dropped{}",
              overwritten_render_targets, overwrite_rectangle.x_pixels, overwrite_rectangle.y_pixels,
              overwrite_rectangle.width_pixels, overwrite_rectangle.height_pixels, dropped_count,
              defer ? ", the rest held back" : "");
        }
        PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets, kept_render_targets,
                                         kept_transfers.data());
        if (defer) {
          deferred_transfer_targets_ = cut_targets;
          std::memcpy(deferred_transfer_bindings_, depth_and_color_render_targets,
                      sizeof(deferred_transfer_bindings_));
          for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
            deferred_transfers_[i] = std::move(cut_transfers[i]);
          }
          deferred_transfer_cutout_ = overwrite_rectangle;
        } else {
          PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets, cut_render_targets,
                                           cut_transfers.data(), nullptr, &overwrite_rectangle);
        }
      } else {
        PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                         depth_and_color_render_targets, transfers);
      }

      if (REXCVAR_GET(native_rt_debug_poison_overwrites)) {
        Transfer::Rectangle poison_rectangle;
        uint32_t poison_targets =
            GetDrawOverwrittenRenderTargets(normalized_depth_control, normalized_color_mask,
                                            vertex_shader, poison_rectangle, nullptr);
        if (poison_targets) {
          RenderTarget* poison_render_targets[1 + xenos::kMaxColorRenderTargets] = {};
          uint64_t poison_values[1 + xenos::kMaxColorRenderTargets];
          std::array<std::vector<Transfer>, 1 + xenos::kMaxColorRenderTargets> no_transfers;
          for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
            if (poison_targets & (uint32_t(1) << i)) {
              poison_render_targets[i] = depth_and_color_render_targets[i];
            }
            poison_values[i] = 0x5A5AA5A55A5AA5A5ull;
          }
          PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                           poison_render_targets, no_transfers.data(),
                                           poison_values, &poison_rectangle);
        }
      }

      if (native_rt_mode_ && depth_and_color_render_targets[0]) {
        UpdateUniformStencil(*static_cast<VulkanRenderTarget*>(depth_and_color_render_targets[0]),
                             normalized_depth_control, normalized_color_mask, vertex_shader);
      }

      if (depth_and_color_render_targets[0]) {
        render_pass_key.depth_and_color_used |= 1 << 0;
        render_pass_key.depth_format = depth_and_color_render_targets[0]->key().GetDepthFormat();
      }
      if (depth_and_color_render_targets[1]) {
        render_pass_key.depth_and_color_used |= 1 << 1;
        render_pass_key.color_0_view_format =
            depth_and_color_render_targets[1]->key().GetColorFormat();
      }
      if (depth_and_color_render_targets[2]) {
        render_pass_key.depth_and_color_used |= 1 << 2;
        render_pass_key.color_1_view_format =
            depth_and_color_render_targets[2]->key().GetColorFormat();
      }
      if (depth_and_color_render_targets[3]) {
        render_pass_key.depth_and_color_used |= 1 << 3;
        render_pass_key.color_2_view_format =
            depth_and_color_render_targets[3]->key().GetColorFormat();
      }
      if (depth_and_color_render_targets[4]) {
        render_pass_key.depth_and_color_used |= 1 << 4;
        render_pass_key.color_3_view_format =
            depth_and_color_render_targets[4]->key().GetColorFormat();
      }

      const Framebuffer* framebuffer = last_update_framebuffer_;
      VkRenderPass render_pass = last_update_render_pass_key_ == render_pass_key
                                     ? last_update_render_pass_
                                     : VK_NULL_HANDLE;
      if (render_pass == VK_NULL_HANDLE) {
        render_pass = GetHostRenderTargetsRenderPass(render_pass_key);
        if (render_pass == VK_NULL_HANDLE) {
          return false;
        }
        // Framebuffer for a different render pass needed now.
        framebuffer = nullptr;
      }

      uint32_t pitch_tiles_at_32bpp =
          ((rb_surface_info.surface_pitch
            << uint32_t(rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X)) +
           (xenos::kEdramTileWidthSamples - 1)) /
          xenos::kEdramTileWidthSamples;
      if (framebuffer) {
        if (last_update_framebuffer_pitch_tiles_at_32bpp_ != pitch_tiles_at_32bpp ||
            std::memcmp(last_update_framebuffer_attachments_, depth_and_color_render_targets,
                        sizeof(last_update_framebuffer_attachments_))) {
          framebuffer = nullptr;
        }
      }
      if (!framebuffer) {
        framebuffer = GetHostRenderTargetsFramebuffer(render_pass_key, pitch_tiles_at_32bpp,
                                                      depth_and_color_render_targets);
        if (!framebuffer) {
          return false;
        }
      }

      // Successful update - write the new configuration.
      last_update_render_pass_key_ = render_pass_key;
      last_update_render_pass_ = render_pass;
      last_update_framebuffer_pitch_tiles_at_32bpp_ = pitch_tiles_at_32bpp;
      std::memcpy(last_update_framebuffer_attachments_, depth_and_color_render_targets,
                  sizeof(last_update_framebuffer_attachments_));
      last_update_framebuffer_ = framebuffer;

      // Transition the used render targets.
      for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
        RenderTarget* rt = depth_and_color_render_targets[i];
        if (!rt) {
          continue;
        }
        auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rt);
        VkPipelineStageFlags rt_dst_stage_mask;
        VkAccessFlags rt_dst_access_mask;
        VkImageLayout rt_new_layout;
        VulkanRenderTarget::GetDrawUsage(i == 0, &rt_dst_stage_mask, &rt_dst_access_mask,
                                         &rt_new_layout);
        command_processor_.PushImageMemoryBarrier(
            vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                i ? VK_IMAGE_ASPECT_COLOR_BIT
                  : (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)),
            vulkan_rt.current_stage_mask(), rt_dst_stage_mask, vulkan_rt.current_access_mask(),
            rt_dst_access_mask, vulkan_rt.current_layout(), rt_new_layout);
        vulkan_rt.SetUsage(rt_dst_stage_mask, rt_dst_access_mask, rt_new_layout);
      }
    } break;

    case Path::kPixelShaderInterlock: {
      // For FSI, only the barrier is needed - already scheduled if required.
      // But the buffer will be used for FSI drawing now.
      UseEdramBuffer(EdramBufferUsage::kFragmentReadWrite);
      // Commit preceding unordered (but not FSI) writes like clears as they
      // aren't synchronized with FSI accesses.
      CommitEdramBufferShaderWrites(EdramBufferModificationStatus::kViaUnordered);
      // TODO(Triang3l): Check if this draw call modifies color or depth /
      // stencil, at least coarsely, to prevent useless barriers.
      MarkEdramBufferModified(EdramBufferModificationStatus::kViaFragmentShaderInterlock);
      last_update_render_pass_key_ = render_pass_key;
      last_update_render_pass_ = fsi_render_pass_;
      last_update_framebuffer_ = &fsi_framebuffer_;
    } break;

    default:
      assert_unhandled_case(GetPath());
      return false;
  }

  return true;
}

void VulkanRenderTargetCache::GetLastUpdateRenderingAttachments(
    VkRenderingAttachmentInfo* color_attachments, uint32_t* color_attachment_count_out,
    VkRenderingAttachmentInfo* depth_attachment,
    VkRenderingAttachmentInfo* stencil_attachment) const {
  RenderPassKey key = last_update_render_pass_key_;
  RenderTarget* const* rts = last_update_accumulated_render_targets();

  std::memset(depth_attachment, 0, sizeof(VkRenderingAttachmentInfo));
  std::memset(stencil_attachment, 0, sizeof(VkRenderingAttachmentInfo));

  if ((key.depth_and_color_used & 0b1) && rts[0]) {
    const auto* vulkan_rt = static_cast<const VulkanRenderTarget*>(rts[0]);
    depth_attachment->sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment->pNext = nullptr;
    depth_attachment->imageView = vulkan_rt->view_depth_stencil();
    depth_attachment->imageLayout = VulkanRenderTarget::kDepthDrawLayout;
    depth_attachment->resolveMode = VK_RESOLVE_MODE_NONE;
    depth_attachment->resolveImageView = VK_NULL_HANDLE;
    depth_attachment->resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_attachment->clearValue = {};
    *stencil_attachment = *depth_attachment;
  }

  uint32_t color_attachment_count = 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    VkRenderingAttachmentInfo& color_attachment = color_attachments[i];
    std::memset(&color_attachment, 0, sizeof(VkRenderingAttachmentInfo));
    if ((key.depth_and_color_used & (1 << (1 + i))) && rts[1 + i]) {
      const auto* vulkan_rt = static_cast<const VulkanRenderTarget*>(rts[1 + i]);
      color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      color_attachment.pNext = nullptr;
      color_attachment.imageView = vulkan_rt->view_depth_color();
      color_attachment.imageLayout = VulkanRenderTarget::kColorDrawLayout;
      color_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
      color_attachment.resolveImageView = VK_NULL_HANDLE;
      color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color_attachment.clearValue = {};
      color_attachment_count = i + 1;
    }
  }
  *color_attachment_count_out = color_attachment_count;
}

VkRenderPass VulkanRenderTargetCache::GetHostRenderTargetsRenderPass(RenderPassKey key) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  auto it = render_passes_.find(key);
  if (it != render_passes_.end()) {
    return it->second;
  }

  VkSampleCountFlagBits samples;
  switch (key.msaa_samples) {
    case xenos::MsaaSamples::k1X:
      samples = VK_SAMPLE_COUNT_1_BIT;
      break;
    case xenos::MsaaSamples::k2X:
      samples = IsMsaa2xSupported(key.depth_and_color_used != 0) ? VK_SAMPLE_COUNT_2_BIT
                                                                 : VK_SAMPLE_COUNT_4_BIT;
      break;
    case xenos::MsaaSamples::k4X:
      samples = VK_SAMPLE_COUNT_4_BIT;
      break;
    default:
      return VK_NULL_HANDLE;
  }

  VkAttachmentDescription attachments[1 + xenos::kMaxColorRenderTargets];
  if (key.depth_and_color_used & 0b1) {
    VkAttachmentDescription& attachment = attachments[0];
    attachment.flags = 0;
    attachment.format = GetDepthVulkanFormat(key.depth_format);
    attachment.samples = samples;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VulkanRenderTarget::kDepthDrawLayout;
    attachment.finalLayout = VulkanRenderTarget::kDepthDrawLayout;
  }
  VkAttachmentReference color_attachments[xenos::kMaxColorRenderTargets];
  xenos::ColorRenderTargetFormat color_formats[] = {
      key.color_0_view_format,
      key.color_1_view_format,
      key.color_2_view_format,
      key.color_3_view_format,
  };
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    VkAttachmentReference& color_attachment = color_attachments[i];
    color_attachment.layout = VulkanRenderTarget::kColorDrawLayout;
    uint32_t attachment_bit = uint32_t(1) << (1 + i);
    if (!(key.depth_and_color_used & attachment_bit)) {
      color_attachment.attachment = VK_ATTACHMENT_UNUSED;
      continue;
    }
    uint32_t attachment_index = rex::bit_count(key.depth_and_color_used & (attachment_bit - 1));
    color_attachment.attachment = attachment_index;
    VkAttachmentDescription& attachment = attachments[attachment_index];
    attachment.flags = 0;
    xenos::ColorRenderTargetFormat color_format = color_formats[i];
    attachment.format = key.color_rts_use_transfer_formats
                            ? GetColorOwnershipTransferVulkanFormat(color_format, key.msaa_samples)
                            : GetColorVulkanFormat(color_format);
    attachment.samples = samples;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VulkanRenderTarget::kColorDrawLayout;
    attachment.finalLayout = VulkanRenderTarget::kColorDrawLayout;
  }

  VkAttachmentReference depth_stencil_attachment;
  depth_stencil_attachment.attachment = (key.depth_and_color_used & 0b1) ? 0 : VK_ATTACHMENT_UNUSED;
  depth_stencil_attachment.layout = VulkanRenderTarget::kDepthDrawLayout;

  VkSubpassDescription subpass;
  subpass.flags = 0;
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.inputAttachmentCount = 0;
  subpass.pInputAttachments = nullptr;
  subpass.colorAttachmentCount = 32 - rex::lzcnt(uint32_t(key.depth_and_color_used >> 1));
  subpass.pColorAttachments = color_attachments;
  subpass.pResolveAttachments = nullptr;
  subpass.pDepthStencilAttachment =
      (key.depth_and_color_used & 0b1) ? &depth_stencil_attachment : nullptr;
  subpass.preserveAttachmentCount = 0;
  subpass.pPreserveAttachments = nullptr;

  VkPipelineStageFlags dependency_stage_mask = 0;
  VkAccessFlags dependency_access_mask = 0;
  if (key.depth_and_color_used & 0b1) {
    dependency_stage_mask |= VulkanRenderTarget::kDepthDrawStageMask;
    dependency_access_mask |= VulkanRenderTarget::kDepthDrawAccessMask;
  }
  if (key.depth_and_color_used >> 1) {
    dependency_stage_mask |= VulkanRenderTarget::kColorDrawStageMask;
    dependency_access_mask |= VulkanRenderTarget::kColorDrawAccessMask;
  }
  VkSubpassDependency subpass_dependencies[2];
  subpass_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  subpass_dependencies[0].dstSubpass = 0;
  subpass_dependencies[0].srcStageMask = dependency_stage_mask;
  subpass_dependencies[0].dstStageMask = dependency_stage_mask;
  subpass_dependencies[0].srcAccessMask = dependency_access_mask;
  subpass_dependencies[0].dstAccessMask = dependency_access_mask;
  subpass_dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
  subpass_dependencies[1].srcSubpass = 0;
  subpass_dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  subpass_dependencies[1].srcStageMask = dependency_stage_mask;
  subpass_dependencies[1].dstStageMask = dependency_stage_mask;
  subpass_dependencies[1].srcAccessMask = dependency_access_mask;
  subpass_dependencies[1].dstAccessMask = dependency_access_mask;
  subpass_dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

  VkRenderPassCreateInfo render_pass_create_info;
  render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  render_pass_create_info.pNext = nullptr;
  render_pass_create_info.flags = 0;
  render_pass_create_info.attachmentCount = rex::bit_count(key.depth_and_color_used);
  render_pass_create_info.pAttachments = attachments;
  render_pass_create_info.subpassCount = 1;
  render_pass_create_info.pSubpasses = &subpass;
  render_pass_create_info.dependencyCount =
      key.depth_and_color_used ? uint32_t(rex::countof(subpass_dependencies)) : 0;
  render_pass_create_info.pDependencies = subpass_dependencies;

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkRenderPass render_pass;
  if (dfn.vkCreateRenderPass(device, &render_pass_create_info, nullptr, &render_pass) !=
      VK_SUCCESS) {
    REXGPU_ERROR("VulkanRenderTargetCache: Failed to create a render pass");
    render_passes_.emplace(key, VK_NULL_HANDLE);
    return VK_NULL_HANDLE;
  }
  render_passes_.emplace(key, render_pass);
  return render_pass;
}

VkFormat VulkanRenderTargetCache::GetDepthVulkanFormat(
    xenos::DepthRenderTargetFormat format) const {
  if (format == xenos::DepthRenderTargetFormat::kD24S8 && depth_unorm24_vulkan_format_supported()) {
    return VK_FORMAT_D24_UNORM_S8_UINT;
  }
  return VK_FORMAT_D32_SFLOAT_S8_UINT;
}

bool VulkanRenderTargetCache::IsColor16FormatFloatLike(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
      return color_rg16_draw_format_fallback_to_float_;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return color_rgba16_draw_format_fallback_to_float_;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return true;
    default:
      return false;
  }
}

VkFormat VulkanRenderTargetCache::GetColorVulkanFormat(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return gamma_render_target_as_unorm16_ ? VK_FORMAT_R16G16B16A16_UNORM
                                             : VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return REXCVAR_GET(vulkan_2_10_10_10_exact) ? VK_FORMAT_A2B10G10R10_UNORM_PACK32
                                                  : VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16:
      return color_rg16_draw_format_fallback_to_float_ ? VK_FORMAT_R16G16_SFLOAT
                                                       : VK_FORMAT_R16G16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return color_rgba16_draw_format_fallback_to_float_ ? VK_FORMAT_R16G16B16A16_SFLOAT
                                                         : VK_FORMAT_R16G16B16A16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      assert_unhandled_case(format);
      return VK_FORMAT_UNDEFINED;
  }
}

VkFormat VulkanRenderTargetCache::GetColorOwnershipTransferVulkanFormat(
    xenos::ColorRenderTargetFormat format, xenos::MsaaSamples msaa_samples,
    bool* is_integer_out) const {
  if (is_integer_out) {
    *is_integer_out = true;
  }
  VkSampleCountFlagBits transfer_sample_count =
      VkSampleCountFlagBits(uint32_t(1) << uint32_t(msaa_samples));
  if (transfer_sample_count == VK_SAMPLE_COUNT_2_BIT && !msaa_2x_attachments_supported_) {
    // 2x guest color render targets are allocated as 4x host render targets in
    // this mode.
    transfer_sample_count = VK_SAMPLE_COUNT_4_BIT;
  }
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();
  bool integer_transfer_sample_count_supported =
      (device_properties.framebufferColorSampleCounts & transfer_sample_count) != 0 &&
      (device_properties.sampledImageIntegerSampleCounts & transfer_sample_count) != 0;
  // Floating-point numbers have NaNs that need to be propagated without
  // modifications to the bit representation, and SNORM has two representations
  // of -1.
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      if (color_16bit_transfer_uint_formats_supported_ && integer_transfer_sample_count_supported) {
        return VK_FORMAT_R16G16_UINT;
      }
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorVulkanFormat(format);
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      if (color_16bit_transfer_uint_formats_supported_ && integer_transfer_sample_count_supported) {
        return VK_FORMAT_R16G16B16A16_UINT;
      }
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorVulkanFormat(format);
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      if (color_32bit_transfer_uint_formats_supported_ && integer_transfer_sample_count_supported) {
        return VK_FORMAT_R32_UINT;
      }
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorVulkanFormat(format);
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      if (color_32bit_transfer_uint_formats_supported_ && integer_transfer_sample_count_supported) {
        return VK_FORMAT_R32G32_UINT;
      }
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorVulkanFormat(format);
    default:
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorVulkanFormat(format);
  }
}

VulkanRenderTargetCache::VulkanRenderTarget::~VulkanRenderTarget() {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      render_target_cache_.command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
      key().is_depth ? *render_target_cache_.descriptor_set_pool_sampled_image_x2_
                     : *render_target_cache_.descriptor_set_pool_sampled_image_;
  descriptor_set_pool.Free(descriptor_set_index_transfer_source_);
  if (view_color_transfer_separate_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_color_transfer_separate_, nullptr);
  }
  if (view_srgb_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_srgb_, nullptr);
  }
  if (view_stencil_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_stencil_, nullptr);
  }
  if (view_depth_stencil_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_depth_stencil_, nullptr);
  }
  dfn.vkDestroyImageView(device, view_depth_color_, nullptr);
  dfn.vkDestroyImage(device, image_, nullptr);
  dfn.vkFreeMemory(device, memory_, nullptr);
}

uint32_t VulkanRenderTargetCache::GetMaxRenderTargetWidth() const {
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();
  return std::min(device_properties.maxFramebufferWidth, device_properties.maxImageDimension2D);
}

uint32_t VulkanRenderTargetCache::GetMaxRenderTargetHeight() const {
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();
  return std::min(device_properties.maxFramebufferHeight, device_properties.maxImageDimension2D);
}

uint32_t VulkanRenderTargetCache::GetFullRenderTargetTileRows(RenderTargetKey key) const {
  return GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples,
                               GetRenderTargetScaleY(key)) /
         (xenos::kEdramTileHeightSamples >> uint32_t(key.msaa_samples >= xenos::MsaaSamples::k2X));
}

RenderTargetCache::RenderTarget* VulkanRenderTargetCache::CreateRenderTarget(RenderTargetKey key) {
  uint32_t tile_rows = GetFullRenderTargetTileRows(key);
  if (REXCVAR_GET(native_rt_size_by_use)) {
    if (render_target_create_tile_rows_) {
      tile_rows = std::min(tile_rows, render_target_create_tile_rows_);
    }
    if (int32_t debug_start_rows = REXCVAR_GET(native_rt_debug_start_tile_rows);
        debug_start_rows > 0) {
      tile_rows = std::min(tile_rows, uint32_t(debug_start_rows));
    }
  }
  VulkanRenderTarget::Image image;
  if (!CreateRenderTargetImage(key, tile_rows, image)) {
    return nullptr;
  }
  if (tile_rows < GetFullRenderTargetTileRows(key)) {
    REXGPU_INFO("VulkanRenderTargetCache: the {} render target at EDRAM base {} covers {} rows of "
                "tiles ({} pixels before scaling)",
                key.is_depth ? "depth" : "color", uint32_t(key.base_tiles), tile_rows,
                tile_rows * (xenos::kEdramTileHeightSamples >>
                             uint32_t(key.msaa_samples >= xenos::MsaaSamples::k2X)));
  }
  return new VulkanRenderTarget(key, *this, image);
}

bool VulkanRenderTargetCache::CreateRenderTargetImage(RenderTargetKey key, uint32_t tile_rows,
                                                      VulkanRenderTarget::Image& image_out) {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Create the image.

  VkImageCreateInfo image_create_info;
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.pNext = nullptr;
  image_create_info.flags = 0;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.extent.width = key.GetWidth() * GetRenderTargetScaleX(key);
  image_create_info.extent.height =
      tile_rows *
      (xenos::kEdramTileHeightSamples >> uint32_t(key.msaa_samples >= xenos::MsaaSamples::k2X)) *
      GetRenderTargetScaleY(key);
  image_create_info.extent.depth = 1;
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  if (key.msaa_samples == xenos::MsaaSamples::k2X && !msaa_2x_attachments_supported_) {
    image_create_info.samples = VK_SAMPLE_COUNT_4_BIT;
  } else {
    image_create_info.samples = VkSampleCountFlagBits(uint32_t(1) << uint32_t(key.msaa_samples));
  }
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  if (REXCVAR_GET(native_rt_size_by_use) || REXCVAR_GET(native_resolve_copy_free) ||
      REXCVAR_GET(native_resolve_image_copies)) {
    // Growing copies the image; textures that take over images are loaded
    // with copies (and have the same usages).
    image_create_info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
  image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create_info.queueFamilyIndexCount = 0;
  image_create_info.pQueueFamilyIndices = nullptr;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkFormat transfer_format;
  bool is_srgb_view_needed = false;
  if (key.is_depth) {
    image_create_info.format = GetDepthVulkanFormat(key.GetDepthFormat());
    transfer_format = image_create_info.format;
    image_create_info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  } else {
    xenos::ColorRenderTargetFormat color_format = key.GetColorFormat();
    image_create_info.format = GetColorVulkanFormat(color_format);
    transfer_format = GetColorOwnershipTransferVulkanFormat(color_format, key.msaa_samples);
    is_srgb_view_needed = false;
    if (image_create_info.format != transfer_format || is_srgb_view_needed) {
      image_create_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    }
    image_create_info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }
  if (image_create_info.format == VK_FORMAT_UNDEFINED) {
    REXGPU_ERROR("VulkanRenderTargetCache: Unknown {} render target format {}",
                 key.is_depth ? "depth" : "color", static_cast<uint32_t>(key.resource_format));
    return false;
  }
  VkImage image;
  VkDeviceMemory memory;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_create_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal, image,
          memory)) {
    REXGPU_ERROR(
        "VulkanRenderTarget: Failed to create a {}x{} {}xMSAA {} render target "
        "image",
        image_create_info.extent.width, image_create_info.extent.height,
        uint32_t(1) << uint32_t(key.msaa_samples), key.GetFormatName());
    return false;
  }

  VkExtent2D extent = {image_create_info.extent.width, image_create_info.extent.height};
  if (!CreateRenderTargetImageViews(key, image, image_create_info.format, transfer_format,
                                    is_srgb_view_needed, extent, image_out)) {
    dfn.vkDestroyImage(device, image, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
    return false;
  }
  image_out.image = image;
  image_out.memory = memory;
  image_out.tile_rows = tile_rows;
  return true;
}

bool VulkanRenderTargetCache::CreateRenderTargetImageViews(
    RenderTargetKey key, VkImage image, VkFormat format, VkFormat transfer_format,
    bool is_srgb_view_needed, const VkExtent2D& extent, VulkanRenderTarget::Image& image_out) {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Create the image views.

  VkImageViewCreateInfo view_create_info;
  view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_create_info.pNext = nullptr;
  view_create_info.flags = 0;
  view_create_info.image = image;
  view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_create_info.format = format;
  view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange(
      key.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageView view_depth_color;
  if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &view_depth_color) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTarget: Failed to create a {} view for a {}x{} {}xMSAA {} "
        "render target",
        key.is_depth ? "depth" : "color", extent.width, extent.height,
        uint32_t(1) << uint32_t(key.msaa_samples), key.GetFormatName());
    return false;
  }
  VkImageView view_depth_stencil = VK_NULL_HANDLE;
  VkImageView view_stencil = VK_NULL_HANDLE;
  VkImageView view_srgb = VK_NULL_HANDLE;
  VkImageView view_color_transfer_separate = VK_NULL_HANDLE;
  if (key.is_depth) {
    view_create_info.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &view_depth_stencil) !=
        VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTarget: Failed to create a depth / stencil view for a "
          "{}x{} {}xMSAA {} render target",
          extent.width, extent.height, uint32_t(1) << uint32_t(key.msaa_samples),
          xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat()));
      dfn.vkDestroyImageView(device, view_depth_color, nullptr);
      return false;
    }
    view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &view_stencil) != VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTarget: Failed to create a stencil view for a {}x{} "
          "{}xMSAA render target",
          extent.width, extent.height, uint32_t(1) << uint32_t(key.msaa_samples),
          xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat()));
      dfn.vkDestroyImageView(device, view_depth_stencil, nullptr);
      dfn.vkDestroyImageView(device, view_depth_color, nullptr);
      return false;
    }
  } else {
    if (is_srgb_view_needed) {
      view_create_info.format = VK_FORMAT_R8G8B8A8_SRGB;
      if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &view_srgb) != VK_SUCCESS) {
        REXGPU_ERROR(
            "VulkanRenderTarget: Failed to create an sRGB view for a {}x{} "
            "{}xMSAA render target",
            extent.width, extent.height, uint32_t(1) << uint32_t(key.msaa_samples),
            xenos::GetColorRenderTargetFormatName(key.GetColorFormat()));
        dfn.vkDestroyImageView(device, view_depth_color, nullptr);
        return false;
      }
    }
    if (transfer_format != format) {
      view_create_info.format = transfer_format;
      if (dfn.vkCreateImageView(device, &view_create_info, nullptr,
                                &view_color_transfer_separate) != VK_SUCCESS) {
        REXGPU_ERROR(
            "VulkanRenderTarget: Failed to create a transfer view for a {}x{} "
            "{}xMSAA {} render target",
            extent.width, extent.height, uint32_t(1) << uint32_t(key.msaa_samples),
            key.GetFormatName());
        if (view_srgb != VK_NULL_HANDLE) {
          dfn.vkDestroyImageView(device, view_srgb, nullptr);
        }
        dfn.vkDestroyImageView(device, view_depth_color, nullptr);
        return false;
      }
    }
  }

  ui::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
      key.is_depth ? *descriptor_set_pool_sampled_image_x2_ : *descriptor_set_pool_sampled_image_;
  size_t descriptor_set_index_transfer_source = descriptor_set_pool.Allocate();
  if (descriptor_set_index_transfer_source == SIZE_MAX) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to allocate sampled image descriptors "
        "for a {} render target",
        key.is_depth ? "depth/stencil" : "color");
    if (view_color_transfer_separate != VK_NULL_HANDLE) {
      dfn.vkDestroyImageView(device, view_color_transfer_separate, nullptr);
    }
    if (view_srgb != VK_NULL_HANDLE) {
      dfn.vkDestroyImageView(device, view_srgb, nullptr);
    }
    dfn.vkDestroyImageView(device, view_depth_color, nullptr);
    return false;
  }
  VkDescriptorSet descriptor_set_transfer_source =
      descriptor_set_pool.Get(descriptor_set_index_transfer_source);
  VkWriteDescriptorSet descriptor_set_write[2];
  VkDescriptorImageInfo descriptor_set_write_depth_color;
  descriptor_set_write_depth_color.sampler = VK_NULL_HANDLE;
  descriptor_set_write_depth_color.imageView = view_color_transfer_separate != VK_NULL_HANDLE
                                                   ? view_color_transfer_separate
                                                   : view_depth_color;
  descriptor_set_write_depth_color.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  descriptor_set_write[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptor_set_write[0].pNext = nullptr;
  descriptor_set_write[0].dstSet = descriptor_set_transfer_source;
  descriptor_set_write[0].dstBinding = 0;
  descriptor_set_write[0].dstArrayElement = 0;
  descriptor_set_write[0].descriptorCount = 1;
  descriptor_set_write[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptor_set_write[0].pImageInfo = &descriptor_set_write_depth_color;
  descriptor_set_write[0].pBufferInfo = nullptr;
  descriptor_set_write[0].pTexelBufferView = nullptr;
  VkDescriptorImageInfo descriptor_set_write_stencil;
  if (key.is_depth) {
    descriptor_set_write_stencil.sampler = VK_NULL_HANDLE;
    descriptor_set_write_stencil.imageView = view_stencil;
    descriptor_set_write_stencil.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    descriptor_set_write[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write[1].pNext = nullptr;
    descriptor_set_write[1].dstSet = descriptor_set_transfer_source;
    descriptor_set_write[1].dstBinding = 1;
    descriptor_set_write[1].dstArrayElement = 0;
    descriptor_set_write[1].descriptorCount = 1;
    descriptor_set_write[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_write[1].pImageInfo = &descriptor_set_write_stencil;
    descriptor_set_write[1].pBufferInfo = nullptr;
    descriptor_set_write[1].pTexelBufferView = nullptr;
  }
  dfn.vkUpdateDescriptorSets(device, key.is_depth ? 2 : 1, descriptor_set_write, 0, nullptr);

  image_out.view_depth_color = view_depth_color;
  image_out.view_depth_stencil = view_depth_stencil;
  image_out.view_stencil = view_stencil;
  image_out.view_srgb = view_srgb;
  image_out.view_color_transfer_separate = view_color_transfer_separate;
  image_out.descriptor_set_index_transfer_source = descriptor_set_index_transfer_source;
  return true;
  return true;
}

bool VulkanRenderTargetCache::CreateRenderTargetImageViewsForKey(
    RenderTargetKey key, VkImage image, const VkExtent2D& extent,
    VulkanRenderTarget::Image& image_out) {
  VkFormat format, transfer_format;
  if (key.is_depth) {
    format = GetDepthVulkanFormat(key.GetDepthFormat());
    transfer_format = format;
  } else {
    format = GetColorVulkanFormat(key.GetColorFormat());
    transfer_format = GetColorOwnershipTransferVulkanFormat(key.GetColorFormat(), key.msaa_samples);
  }
  return CreateRenderTargetImageViews(key, image, format, transfer_format, false, extent,
                                      image_out);
}

void VulkanRenderTargetCache::DestroyRenderTargetImage(bool is_depth,
                                                       const VulkanRenderTarget::Image& image) {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
      is_depth ? *descriptor_set_pool_sampled_image_x2_ : *descriptor_set_pool_sampled_image_;
  descriptor_set_pool.Free(image.descriptor_set_index_transfer_source);
  for (VkImageView view : {image.view_color_transfer_separate, image.view_srgb,
                           image.view_stencil, image.view_depth_stencil,
                           image.view_depth_color}) {
    if (view != VK_NULL_HANDLE) {
      dfn.vkDestroyImageView(device, view, nullptr);
    }
  }
  dfn.vkDestroyImage(device, image.image, nullptr);
  dfn.vkFreeMemory(device, image.memory, nullptr);
}

void VulkanRenderTargetCache::DestroyRetiredRenderTargetObjects(uint64_t completed_submission) {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  while (!retired_render_target_images_.empty() &&
         retired_render_target_images_.front().submission <= completed_submission) {
    const RetiredRenderTargetImage& retired = retired_render_target_images_.front();
    DestroyRenderTargetImage(retired.is_depth, retired.image);
    retired_render_target_images_.pop_front();
  }
  while (!retired_framebuffers_.empty() &&
         retired_framebuffers_.front().first <= completed_submission) {
    dfn.vkDestroyFramebuffer(device, retired_framebuffers_.front().second, nullptr);
    retired_framebuffers_.pop_front();
  }
}

void VulkanRenderTargetCache::EnsureRenderTargetTileRows(RenderTargetKey key,
                                                         uint32_t tile_rows) {
  if (!REXCVAR_GET(native_rt_size_by_use)) {
    return;
  }
  auto* render_target = static_cast<VulkanRenderTarget*>(FindRenderTarget(key));
  if (!render_target) {
    return;
  }
  tile_rows = std::min(tile_rows, GetFullRenderTargetTileRows(key));
  if (render_target->tile_rows() >= tile_rows) {
    int32_t regrow_interval = REXCVAR_GET(native_rt_debug_regrow_interval);
    if (regrow_interval <= 0 ||
        ++render_target->debug_regrow_counter() % uint32_t(regrow_interval)) {
      return;
    }
    // Testing: replace the image anyway.
    tile_rows = render_target->tile_rows();
  }
  VulkanRenderTarget::Image new_image;
  if (!CreateRenderTargetImage(key, tile_rows, new_image)) {
    // Keeps drawing to the rows it has.
    return;
  }
  if (tile_rows != render_target->tile_rows()) {
    REXGPU_INFO("VulkanRenderTargetCache: growing the {} render target at EDRAM base {} (pitch "
                "{} tiles) from {} to {} rows of tiles",
                key.is_depth ? "depth" : "color", uint32_t(key.base_tiles),
                uint32_t(key.pitch_tiles_at_32bpp), render_target->tile_rows(), tile_rows);
  }

  // Copy the contents of the old image to the new one, outside render passes.
  command_processor_.EndRenderPass();
  VkImageSubresourceRange subresource_range = ui::vulkan::util::InitializeSubresourceRange(
      key.is_depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                   : VK_IMAGE_ASPECT_COLOR_BIT);
  command_processor_.PushImageMemoryBarrier(
      render_target->image(), subresource_range, render_target->current_stage_mask(),
      VK_PIPELINE_STAGE_TRANSFER_BIT, render_target->current_access_mask(),
      VK_ACCESS_TRANSFER_READ_BIT, render_target->current_layout(),
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  command_processor_.PushImageMemoryBarrier(
      new_image.image, subresource_range, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  command_processor_.SubmitBarriers(true);
  VkImageCopy region = {};
  region.srcSubresource.aspectMask = subresource_range.aspectMask;
  region.srcSubresource.layerCount = 1;
  region.dstSubresource = region.srcSubresource;
  region.extent.width = key.GetWidth() * GetRenderTargetScaleX(key);
  region.extent.height =
      render_target->tile_rows() *
      (xenos::kEdramTileHeightSamples >> uint32_t(key.msaa_samples >= xenos::MsaaSamples::k2X)) *
      GetRenderTargetScaleY(key);
  region.extent.depth = 1;
  command_processor_.deferred_command_buffer().CmdVkCopyImage(
      render_target->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, new_image.image,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  uint64_t submission = command_processor_.GetCurrentSubmission();
  RetiredRenderTargetImage& retired = retired_render_target_images_.emplace_back();
  retired.submission = submission;
  retired.is_depth = key.is_depth;
  retired.image = render_target->ReplaceImage(new_image);
  render_target->SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  // Framebuffers refer to the views and have the old size.
  for (const auto& framebuffer_pair : framebuffers_) {
    retired_framebuffers_.emplace_back(submission, framebuffer_pair.second.framebuffer);
  }
  framebuffers_.clear();
  last_update_framebuffer_ = nullptr;
}

bool VulkanRenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return !depth_unorm24_vulkan_format_supported();
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return !depth_float24_convert_in_pixel_shader_;
  }
  return false;
}

bool VulkanRenderTargetCache::IsGammaFormatHostStorageSeparate() const {
  return gamma_render_target_as_unorm16_;
}

void VulkanRenderTargetCache::RequestPixelShaderInterlockBarrier() {
  // Keep parity with D3D12 ROV interlock barrier requests by committing any
  // pending EDRAM shader writes, not only FSI writes.
  CommitEdramBufferShaderWrites();
}

void VulkanRenderTargetCache::GetEdramBufferUsageMasks(EdramBufferUsage usage,
                                                       VkPipelineStageFlags& stage_mask_out,
                                                       VkAccessFlags& access_mask_out) {
  switch (usage) {
    case EdramBufferUsage::kFragmentRead:
      stage_mask_out = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT;
      break;
    case EdramBufferUsage::kFragmentReadWrite:
      stage_mask_out = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      break;
    case EdramBufferUsage::kComputeRead:
      stage_mask_out = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT;
      break;
    case EdramBufferUsage::kComputeWrite:
      stage_mask_out = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_WRITE_BIT;
      break;
    case EdramBufferUsage::kTransferRead:
      stage_mask_out = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask_out = VK_ACCESS_TRANSFER_READ_BIT;
      break;
    case EdramBufferUsage::kTransferWrite:
      stage_mask_out = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask_out = VK_ACCESS_TRANSFER_WRITE_BIT;
      break;
    default:
      assert_unhandled_case(usage);
  }
}

void VulkanRenderTargetCache::UseEdramBuffer(EdramBufferUsage new_usage) {
  // HAND PATCH: zero-initialize the EDRAM buffer the first time anything
  // uses it (uninitialized VRAM reads showed as green static in the
  // in-engine cutscenes / intro logos). Recorded through the deferred
  // command buffer, so this happens inside the first real submission.
  if (!edram_buffer_initial_cleared_) {
    edram_buffer_initial_cleared_ = true;
    command_processor_.PushBufferMemoryBarrier(
        edram_buffer_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    command_processor_.SubmitBarriers(true);
    command_processor_.deferred_command_buffer().CmdVkFillBuffer(edram_buffer_, 0, VK_WHOLE_SIZE,
                                                                 0);
    command_processor_.PushBufferMemoryBarrier(
        edram_buffer_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
  }
  if (edram_buffer_usage_ == new_usage) {
    return;
  }
  VkPipelineStageFlags src_stage_mask, dst_stage_mask;
  VkAccessFlags src_access_mask, dst_access_mask;
  GetEdramBufferUsageMasks(edram_buffer_usage_, src_stage_mask, src_access_mask);
  GetEdramBufferUsageMasks(new_usage, dst_stage_mask, dst_access_mask);
  if (command_processor_.PushBufferMemoryBarrier(edram_buffer_, 0, VK_WHOLE_SIZE, src_stage_mask,
                                                 dst_stage_mask, src_access_mask,
                                                 dst_access_mask)) {
    // Resetting edram_buffer_modification_status_ only if the barrier has been
    // truly inserted.
    edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
  }
  edram_buffer_usage_ = new_usage;
}

void VulkanRenderTargetCache::MarkEdramBufferModified(
    EdramBufferModificationStatus modification_status) {
  assert_true(modification_status != EdramBufferModificationStatus::kUnmodified);
  switch (edram_buffer_usage_) {
    case EdramBufferUsage::kFragmentReadWrite:
      // max because being modified via unordered access requires stricter
      // synchronization than via fragment shader interlocks.
      edram_buffer_modification_status_ =
          std::max(edram_buffer_modification_status_, modification_status);
      break;
    case EdramBufferUsage::kComputeWrite:
      assert_true(modification_status == EdramBufferModificationStatus::kViaUnordered);
      modification_status = EdramBufferModificationStatus::kViaUnordered;
      edram_buffer_modification_status_ =
          std::max(edram_buffer_modification_status_, modification_status);
      break;
    default:
      assert_always(
          "While changing the usage of the EDRAM buffer before marking it as "
          "modified is handled safely (but will cause spurious marking as "
          "modified after the changes have been implicitly committed by the "
          "usage switch), normally that shouldn't be done and is an "
          "indication of architectural mistakes. Alternatively, this may "
          "indicate that the usage switch has been forgotten before writing, "
          "which is a clearly invalid situation.");
  }
}

void VulkanRenderTargetCache::CommitEdramBufferShaderWrites(
    EdramBufferModificationStatus commit_status) {
  assert_true(commit_status != EdramBufferModificationStatus::kUnmodified);
  if (edram_buffer_modification_status_ < commit_status) {
    return;
  }
  VkPipelineStageFlags stage_mask;
  VkAccessFlags access_mask;
  GetEdramBufferUsageMasks(edram_buffer_usage_, stage_mask, access_mask);
  if (!(access_mask & VK_ACCESS_SHADER_WRITE_BIT)) {
    // Keep behavior robust similarly to D3D12 when an unexpected state is
    // encountered: avoid emitting an invalid barrier but still treat writes as
    // committed for ownership tracking.
    assert_always("EDRAM writes committed from a non-shader-write usage");
    edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
    PixelShaderInterlockFullEdramBarrierPlaced();
    return;
  }
  command_processor_.PushBufferMemoryBarrier(
      edram_buffer_, 0, VK_WHOLE_SIZE, stage_mask, stage_mask, access_mask, access_mask,
      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
  PixelShaderInterlockFullEdramBarrierPlaced();
}

const VulkanRenderTargetCache::Framebuffer*
VulkanRenderTargetCache::GetHostRenderTargetsFramebuffer(
    RenderPassKey render_pass_key, uint32_t pitch_tiles_at_32bpp,
    const RenderTarget* const* depth_and_color_render_targets) {
  FramebufferKey key;
  key.render_pass_key = render_pass_key;
  key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
  if (render_pass_key.depth_and_color_used & (1 << 0)) {
    key.depth_base_tiles = depth_and_color_render_targets[0]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 1)) {
    key.color_0_base_tiles = depth_and_color_render_targets[1]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 2)) {
    key.color_1_base_tiles = depth_and_color_render_targets[2]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 3)) {
    key.color_2_base_tiles = depth_and_color_render_targets[3]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 4)) {
    key.color_3_base_tiles = depth_and_color_render_targets[4]->key().base_tiles;
  }
  // Original resolution render targets are only bound with each other.
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if ((render_pass_key.depth_and_color_used & (1 << i)) &&
        depth_and_color_render_targets[i]->key().original_resolution) {
      key.original_resolution = 1;
    }
  }
  auto it = framebuffers_.find(key);
  if (it != framebuffers_.end()) {
    return &it->second;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();

  VkRenderPass render_pass = GetHostRenderTargetsRenderPass(render_pass_key);
  if (render_pass == VK_NULL_HANDLE) {
    return nullptr;
  }

  VkImageView attachments[1 + xenos::kMaxColorRenderTargets];
  uint32_t attachment_count = 0;
  uint32_t depth_and_color_rts_remaining = render_pass_key.depth_and_color_used;
  uint32_t rt_index;
  while (rex::bit_scan_forward(depth_and_color_rts_remaining, &rt_index)) {
    depth_and_color_rts_remaining &= ~(uint32_t(1) << rt_index);
    const auto& vulkan_rt =
        *static_cast<const VulkanRenderTarget*>(depth_and_color_render_targets[rt_index]);
    VkImageView attachment;
    if (rt_index) {
      attachment = render_pass_key.color_rts_use_transfer_formats ? vulkan_rt.view_color_transfer()
                                                                  : vulkan_rt.view_depth_color();
    } else {
      attachment = vulkan_rt.view_depth_stencil();
    }
    attachments[attachment_count++] = attachment;
  }

  VkFramebufferCreateInfo framebuffer_create_info;
  framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebuffer_create_info.pNext = nullptr;
  framebuffer_create_info.flags = 0;
  framebuffer_create_info.renderPass = render_pass;
  framebuffer_create_info.attachmentCount = attachment_count;
  framebuffer_create_info.pAttachments = attachments;
  VkExtent2D host_extent;
  if (pitch_tiles_at_32bpp) {
    host_extent.width =
        RenderTargetKey::GetWidth(pitch_tiles_at_32bpp, render_pass_key.msaa_samples);
    host_extent.height = GetRenderTargetHeight(pitch_tiles_at_32bpp, render_pass_key.msaa_samples,
                                               key.original_resolution ? 1 : 0);
    if (REXCVAR_GET(native_rt_size_by_use)) {
      // The attachments may cover fewer rows.
      uint32_t tile_height_pixels =
          xenos::kEdramTileHeightSamples >>
          uint32_t(render_pass_key.msaa_samples >= xenos::MsaaSamples::k2X);
      uint32_t rts_remaining = render_pass_key.depth_and_color_used;
      uint32_t rt_bit;
      while (rex::bit_scan_forward(rts_remaining, &rt_bit)) {
        rts_remaining &= ~(uint32_t(1) << rt_bit);
        host_extent.height = std::min(
            host_extent.height,
            static_cast<const VulkanRenderTarget*>(depth_and_color_render_targets[rt_bit])
                    ->tile_rows() *
                tile_height_pixels);
      }
    }
  } else {
    assert_zero(render_pass_key.depth_and_color_used);
    // Still needed for occlusion queries.
    host_extent.width = xenos::kTexture2DCubeMaxWidthHeight;
    host_extent.height = xenos::kTexture2DCubeMaxWidthHeight;
  }
  // Limiting to the device limit for the case of no attachments, for which
  // there's no limit imposed by the sizes of the attachments that have been
  // created successfully.
  host_extent.width =
      std::min(host_extent.width * (key.original_resolution ? 1 : draw_resolution_scale_x()),
               device_properties.maxFramebufferWidth);
  host_extent.height =
      std::min(host_extent.height * (key.original_resolution ? 1 : draw_resolution_scale_y()),
               device_properties.maxFramebufferHeight);
  framebuffer_create_info.width = host_extent.width;
  framebuffer_create_info.height = host_extent.height;
  framebuffer_create_info.layers = 1;
  VkFramebuffer framebuffer;
  if (dfn.vkCreateFramebuffer(device, &framebuffer_create_info, nullptr, &framebuffer) !=
      VK_SUCCESS) {
    return nullptr;
  }
  // Creates at a persistent location - safe to use pointers.
  return &framebuffers_
              .emplace(std::piecewise_construct, std::forward_as_tuple(key),
                       std::forward_as_tuple(framebuffer, host_extent))
              .first->second;
}

VkShaderModule VulkanRenderTargetCache::GetTransferShader(TransferShaderKey key) {
  auto shader_it = transfer_shaders_.find(key);
  if (shader_it != transfer_shaders_.end()) {
    return shader_it->second;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();

  std::vector<spv::Id> id_vector_temp;
  std::vector<unsigned int> uint_vector_temp;

  SpirvBuilder builder(spv::Spv_1_0, (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1, nullptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityShader);
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_bool = builder.makeBoolType();
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_int2 = builder.makeVectorType(type_int, 2);
  spv::Id type_uint = builder.makeUintType(32);
  spv::Id type_uint2 = builder.makeVectorType(type_uint, 2);
  spv::Id type_uint4 = builder.makeVectorType(type_uint, 4);
  spv::Id type_float = builder.makeFloatType(32);
  spv::Id type_float2 = builder.makeVectorType(type_float, 2);
  spv::Id type_float4 = builder.makeVectorType(type_float, 4);

  const TransferModeInfo& mode = kTransferModes[size_t(key.mode)];
  const TransferPipelineLayoutInfo& pipeline_layout_info =
      kTransferPipelineLayoutInfos[size_t(mode.pipeline_layout)];

  // If not dest_is_color, it's depth, or stencil bit - 40-sample columns are
  // swapped as opposed to color source.
  bool dest_is_color = (mode.output == TransferOutput::kColor);
  xenos::ColorRenderTargetFormat dest_color_format =
      xenos::ColorRenderTargetFormat(key.dest_resource_format);
  xenos::DepthRenderTargetFormat dest_depth_format =
      xenos::DepthRenderTargetFormat(key.dest_resource_format);
  bool dest_is_64bpp = dest_is_color && xenos::IsColorRenderTargetFormat64bpp(dest_color_format);

  xenos::ColorRenderTargetFormat source_color_format =
      xenos::ColorRenderTargetFormat(key.source_resource_format);
  xenos::DepthRenderTargetFormat source_depth_format =
      xenos::DepthRenderTargetFormat(key.source_resource_format);
  // If not source_is_color, it's depth / stencil - 40-sample columns are
  // swapped as opposed to color destination.
  bool source_is_color =
      (pipeline_layout_info.used_descriptor_sets & kTransferUsedDescriptorSetColorTextureBit) != 0;
  bool source_is_64bpp;
  uint32_t source_color_format_component_count;
  uint32_t source_color_texture_component_mask;
  bool source_color_is_uint;
  spv::Id source_color_component_type;
  if (source_is_color) {
    assert_zero(pipeline_layout_info.used_descriptor_sets &
                kTransferUsedDescriptorSetDepthStencilTexturesBit);
    source_is_64bpp = xenos::IsColorRenderTargetFormat64bpp(source_color_format);
    source_color_format_component_count =
        xenos::GetColorRenderTargetFormatComponentCount(source_color_format);
    if (mode.output == TransferOutput::kStencilBit) {
      if (source_is_64bpp && !dest_is_64bpp) {
        // Need one component, but choosing from the two 32bpp halves of the
        // 64bpp sample.
        source_color_texture_component_mask =
            0b1 | (0b1 << (source_color_format_component_count >> 1));
      } else {
        // Red is at least 8 bits per component in all formats.
        source_color_texture_component_mask = 0b1;
      }
    } else {
      source_color_texture_component_mask =
          (uint32_t(1) << source_color_format_component_count) - 1;
    }
    GetColorOwnershipTransferVulkanFormat(source_color_format, key.source_msaa_samples,
                                          &source_color_is_uint);
    source_color_component_type = source_color_is_uint ? type_uint : type_float;
  } else {
    source_is_64bpp = false;
    source_color_format_component_count = 0;
    source_color_texture_component_mask = 0;
    source_color_is_uint = false;
    source_color_component_type = spv::NoType;
  }

  std::vector<spv::Id> main_interface;

  // Outputs.
  bool shader_uses_stencil_reference_output =
      mode.output == TransferOutput::kDepth &&
      vulkan_device->extensions().ext_EXT_shader_stencil_export;
  bool dest_color_is_uint = false;
  uint32_t dest_color_component_count = 0;
  spv::Id type_fragment_data_component = spv::NoResult;
  spv::Id type_fragment_data = spv::NoResult;
  spv::Id output_fragment_data = spv::NoResult;
  spv::Id output_fragment_depth = spv::NoResult;
  spv::Id output_fragment_stencil_ref = spv::NoResult;
  switch (mode.output) {
    case TransferOutput::kColor:
      GetColorOwnershipTransferVulkanFormat(dest_color_format, key.dest_msaa_samples,
                                            &dest_color_is_uint);
      dest_color_component_count =
          xenos::GetColorRenderTargetFormatComponentCount(dest_color_format);
      type_fragment_data_component = dest_color_is_uint ? type_uint : type_float;
      type_fragment_data =
          dest_color_component_count > 1
              ? builder.makeVectorType(type_fragment_data_component, dest_color_component_count)
              : type_fragment_data_component;
      output_fragment_data =
          builder.createVariable(spv::NoPrecision, spv::StorageClassOutput, type_fragment_data,
                                 "xe_transfer_fragment_data");
      builder.addDecoration(output_fragment_data, spv::DecorationLocation, key.dest_color_rt_index);
      main_interface.push_back(output_fragment_data);
      break;
    case TransferOutput::kDepth:
      output_fragment_depth = builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                                     type_float, "gl_FragDepth");
      builder.addDecoration(output_fragment_depth, spv::DecorationBuiltIn, spv::BuiltInFragDepth);
      main_interface.push_back(output_fragment_depth);
      if (shader_uses_stencil_reference_output) {
        builder.addExtension("SPV_EXT_shader_stencil_export");
        builder.addCapability(spv::CapabilityStencilExportEXT);
        output_fragment_stencil_ref = builder.createVariable(
            spv::NoPrecision, spv::StorageClassOutput, type_int, "gl_FragStencilRefARB");
        builder.addDecoration(output_fragment_stencil_ref, spv::DecorationBuiltIn,
                              spv::BuiltInFragStencilRefEXT);
        main_interface.push_back(output_fragment_stencil_ref);
      }
      break;
    default:
      break;
  }

  // Bindings.
  // Generating SPIR-V 1.0, no need to add bindings to the entry point's
  // interface until SPIR-V 1.4.
  // Color source.
  bool source_is_multisampled = key.source_msaa_samples != xenos::MsaaSamples::k1X;
  spv::Id source_color_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets & kTransferUsedDescriptorSetColorTextureBit) {
    source_color_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(source_color_component_type, spv::Dim2D, false, false,
                              source_is_multisampled, 1, spv::ImageFormatUnknown),
        "xe_transfer_color");
    builder.addDecoration(source_color_texture, spv::DecorationDescriptorSet,
                          rex::bit_count(pipeline_layout_info.used_descriptor_sets &
                                         (kTransferUsedDescriptorSetColorTextureBit - 1)));
    builder.addDecoration(source_color_texture, spv::DecorationBinding, 0);
  }
  // Depth / stencil source.
  spv::Id source_depth_texture = spv::NoResult;
  spv::Id source_stencil_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetDepthStencilTexturesBit) {
    uint32_t source_depth_stencil_descriptor_set =
        rex::bit_count(pipeline_layout_info.used_descriptor_sets &
                       (kTransferUsedDescriptorSetDepthStencilTexturesBit - 1));
    // Using `depth == false` in makeImageType because comparisons are not
    // required, and other values of `depth` are causing issues in drivers.
    // https://github.com/microsoft/DirectXShaderCompiler/issues/1107
    if (mode.output != TransferOutput::kStencilBit) {
      source_depth_texture = builder.createVariable(
          spv::NoPrecision, spv::StorageClassUniformConstant,
          builder.makeImageType(type_float, spv::Dim2D, false, false, source_is_multisampled, 1,
                                spv::ImageFormatUnknown),
          "xe_transfer_depth");
      builder.addDecoration(source_depth_texture, spv::DecorationDescriptorSet,
                            source_depth_stencil_descriptor_set);
      builder.addDecoration(source_depth_texture, spv::DecorationBinding, 0);
    }
    if (mode.output != TransferOutput::kDepth || shader_uses_stencil_reference_output) {
      source_stencil_texture = builder.createVariable(
          spv::NoPrecision, spv::StorageClassUniformConstant,
          builder.makeImageType(type_uint, spv::Dim2D, false, false, source_is_multisampled, 1,
                                spv::ImageFormatUnknown),
          "xe_transfer_stencil");
      builder.addDecoration(source_stencil_texture, spv::DecorationDescriptorSet,
                            source_depth_stencil_descriptor_set);
      builder.addDecoration(source_stencil_texture, spv::DecorationBinding, 1);
    }
  }
  // Host depth source buffer.
  spv::Id host_depth_source_buffer = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets & kTransferUsedDescriptorSetHostDepthBufferBit) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeRuntimeArray(type_uint));
    // Storage buffers have std430 packing, no padding to 4-component vectors.
    builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride, sizeof(uint32_t));
    spv::Id type_host_depth_source_buffer =
        builder.makeStructType(id_vector_temp, "XeTransferHostDepthBuffer");
    builder.addMemberName(type_host_depth_source_buffer, 0, "host_depth");
    builder.addMemberDecoration(type_host_depth_source_buffer, 0, spv::DecorationNonWritable);
    builder.addMemberDecoration(type_host_depth_source_buffer, 0, spv::DecorationOffset, 0);
    // Block since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // BufferBlock.
    builder.addDecoration(type_host_depth_source_buffer, spv::DecorationBufferBlock);
    // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // Uniform.
    host_depth_source_buffer =
        builder.createVariable(spv::NoPrecision, spv::StorageClassUniform,
                               type_host_depth_source_buffer, "xe_transfer_host_depth_buffer");
    builder.addDecoration(host_depth_source_buffer, spv::DecorationDescriptorSet,
                          rex::bit_count(pipeline_layout_info.used_descriptor_sets &
                                         (kTransferUsedDescriptorSetHostDepthBufferBit - 1)));
    builder.addDecoration(host_depth_source_buffer, spv::DecorationBinding, 0);
  }
  // Host depth source texture (the depth / stencil descriptor set is reused,
  // but stencil is not needed).
  spv::Id host_depth_source_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
    host_depth_source_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(type_float, spv::Dim2D, false, false,
                              key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X, 1,
                              spv::ImageFormatUnknown),
        "xe_transfer_host_depth");
    builder.addDecoration(
        host_depth_source_texture, spv::DecorationDescriptorSet,
        rex::bit_count(pipeline_layout_info.used_descriptor_sets &
                       (kTransferUsedDescriptorSetHostDepthStencilTexturesBit - 1)));
    builder.addDecoration(host_depth_source_texture, spv::DecorationBinding, 0);
  }
  // Push constants.
  id_vector_temp.clear();
  uint32_t push_constants_member_host_depth_address = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords &
      kTransferUsedPushConstantDwordHostDepthAddressBit) {
    push_constants_member_host_depth_address = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  uint32_t push_constants_member_address = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords & kTransferUsedPushConstantDwordAddressBit) {
    push_constants_member_address = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  uint32_t push_constants_member_stencil_mask = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords &
      kTransferUsedPushConstantDwordStencilMaskBit) {
    push_constants_member_stencil_mask = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  spv::Id push_constants = spv::NoResult;
  if (!id_vector_temp.empty()) {
    spv::Id type_push_constants = builder.makeStructType(id_vector_temp, "XeTransferPushConstants");
    if (pipeline_layout_info.used_push_constant_dwords &
        kTransferUsedPushConstantDwordHostDepthAddressBit) {
      assert_true(push_constants_member_host_depth_address != UINT32_MAX);
      builder.addMemberName(type_push_constants, push_constants_member_host_depth_address,
                            "host_depth_address");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_host_depth_address, spv::DecorationOffset,
          sizeof(uint32_t) *
              rex::bit_count(pipeline_layout_info.used_push_constant_dwords &
                             (kTransferUsedPushConstantDwordHostDepthAddressBit - 1)));
    }
    if (pipeline_layout_info.used_push_constant_dwords & kTransferUsedPushConstantDwordAddressBit) {
      assert_true(push_constants_member_address != UINT32_MAX);
      builder.addMemberName(type_push_constants, push_constants_member_address, "address");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_address, spv::DecorationOffset,
          sizeof(uint32_t) * rex::bit_count(pipeline_layout_info.used_push_constant_dwords &
                                            (kTransferUsedPushConstantDwordAddressBit - 1)));
    }
    if (pipeline_layout_info.used_push_constant_dwords &
        kTransferUsedPushConstantDwordStencilMaskBit) {
      assert_true(push_constants_member_stencil_mask != UINT32_MAX);
      builder.addMemberName(type_push_constants, push_constants_member_stencil_mask,
                            "stencil_mask");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_stencil_mask, spv::DecorationOffset,
          sizeof(uint32_t) * rex::bit_count(pipeline_layout_info.used_push_constant_dwords &
                                            (kTransferUsedPushConstantDwordStencilMaskBit - 1)));
    }
    builder.addDecoration(type_push_constants, spv::DecorationBlock);
    push_constants = builder.createVariable(spv::NoPrecision, spv::StorageClassPushConstant,
                                            type_push_constants, "xe_transfer_push_constants");
  }

  // Coordinate inputs.
  spv::Id input_fragment_coord =
      builder.createVariable(spv::NoPrecision, spv::StorageClassInput, type_float4, "gl_FragCoord");
  builder.addDecoration(input_fragment_coord, spv::DecorationBuiltIn, spv::BuiltInFragCoord);
  main_interface.push_back(input_fragment_coord);
  spv::Id input_sample_id = spv::NoResult;
  spv::Id spec_const_sample_id = spv::NoResult;
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
    if (device_properties.sampleRateShading) {
      // One draw for all samples.
      builder.addCapability(spv::CapabilitySampleRateShading);
      input_sample_id =
          builder.createVariable(spv::NoPrecision, spv::StorageClassInput, type_int, "gl_SampleID");
      builder.addDecoration(input_sample_id, spv::DecorationFlat);
      builder.addDecoration(input_sample_id, spv::DecorationBuiltIn, spv::BuiltInSampleId);
      main_interface.push_back(input_sample_id);
    } else {
      // One sample per draw, with different sample masks.
      spec_const_sample_id = builder.makeUintConstant(0, true);
      builder.addName(spec_const_sample_id, "xe_transfer_sample_id");
      builder.addDecoration(spec_const_sample_id, spv::DecorationSpecId, 0);
    }
  }

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function = builder.makeFunctionEntry(
      spv::NoPrecision, type_void, "main", main_param_types, main_precisions, &main_entry);

  // Working with unsigned numbers for simplicity now, bitcasting to signed will
  // be done at texture fetch.

  uint32_t tile_width_samples = xenos::kEdramTileWidthSamples * draw_resolution_scale_x();
  uint32_t tile_height_samples = xenos::kEdramTileHeightSamples * draw_resolution_scale_y();

  // Split the destination pixel index into 32bpp tile and 32bpp-tile-relative
  // pixel index.
  // Note that division by non-power-of-two constants will include a 4-cycle
  // 32*32 multiplication on AMD, even though so many bits are not needed for
  // the pixel position - however, if an OpUnreachable path is inserted for the
  // case when the position has upper bits set, for some reason, the code for it
  // is not eliminated when compiling the shader for AMD via RenderDoc on
  // Windows, as of June 2022.
  uint_vector_temp.clear();
  uint_vector_temp.push_back(0);
  uint_vector_temp.push_back(1);
  spv::Id dest_pixel_coord = builder.createUnaryOp(
      spv::OpConvertFToU, type_uint2,
      builder.createRvalueSwizzle(spv::NoPrecision, type_float2,
                                  builder.createLoad(input_fragment_coord, spv::NoPrecision),
                                  uint_vector_temp));
  spv::Id dest_pixel_x = builder.createCompositeExtract(dest_pixel_coord, type_uint, 0);
  spv::Id const_dest_tile_width_pixels = builder.makeUintConstant(
      tile_width_samples >>
      (uint32_t(dest_is_64bpp) + uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k4X)));
  spv::Id dest_tile_index_x =
      builder.createBinOp(spv::OpUDiv, type_uint, dest_pixel_x, const_dest_tile_width_pixels);
  spv::Id dest_tile_pixel_x =
      builder.createBinOp(spv::OpUMod, type_uint, dest_pixel_x, const_dest_tile_width_pixels);
  spv::Id dest_pixel_y = builder.createCompositeExtract(dest_pixel_coord, type_uint, 1);
  spv::Id const_dest_tile_height_pixels = builder.makeUintConstant(
      tile_height_samples >> uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k2X));
  spv::Id dest_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, dest_pixel_y, const_dest_tile_height_pixels);
  spv::Id dest_tile_pixel_y =
      builder.createBinOp(spv::OpUMod, type_uint, dest_pixel_y, const_dest_tile_height_pixels);

  assert_true(push_constants_member_address != UINT32_MAX);
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeIntConstant(int32_t(push_constants_member_address)));
  spv::Id address_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants, id_vector_temp),
      spv::NoPrecision);

  // Calculate the 32bpp tile index from its X and Y parts.
  spv::Id dest_tile_index = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint,
          builder.createTriOp(spv::OpBitFieldUExtract, type_uint, address_constant,
                              builder.makeUintConstant(0),
                              builder.makeUintConstant(xenos::kEdramPitchTilesBits)),
          dest_tile_index_y),
      dest_tile_index_x);

  // Load the destination sample index.
  spv::Id dest_sample_id = spv::NoResult;
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
    if (device_properties.sampleRateShading) {
      assert_true(input_sample_id != spv::NoResult);
      dest_sample_id = builder.createUnaryOp(spv::OpBitcast, type_uint,
                                             builder.createLoad(input_sample_id, spv::NoPrecision));
    } else {
      assert_true(spec_const_sample_id != spv::NoResult);
      // Already uint.
      dest_sample_id = spec_const_sample_id;
    }
  }

  // Transform the destination framebuffer pixel and sample coordinates into the
  // source texture pixel and sample coordinates.

  // First sample bit at 4x with Vulkan standard locations - horizontal sample.
  // Second sample bit at 4x with Vulkan standard locations - vertical sample.
  // At 2x:
  // - Native 2x: top is 1 in Vulkan, bottom is 0.
  // - 2x as 4x: top is 0, bottom is 3.

  spv::Id source_sample_id = dest_sample_id;
  spv::Id source_tile_pixel_x = dest_tile_pixel_x;
  spv::Id source_tile_pixel_y = dest_tile_pixel_y;
  spv::Id source_color_half = spv::NoResult;
  if (!source_is_64bpp && dest_is_64bpp) {
    // 32bpp -> 64bpp, need two samples of the source.
    if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 32bpp -> 64bpp, 4x ->.
      // Source has 32bpp halves in two adjacent samples.
      if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 32bpp -> 64bpp, 4x -> 4x.
        // 1 destination horizontal sample = 2 source horizontal samples.
        // D p0,0 s0,0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,0 s1,0 = S p1,0 s0,0 | S p1,0 s1,0
        // D p0,0 s0,1 = S p0,0 s0,1 | S p0,0 s1,1
        // D p0,0 s1,1 = S p1,0 s0,1 | S p1,0 s1,1
        // Thus destination horizontal sample -> source horizontal pixel,
        // vertical samples are 1:1.
        source_sample_id = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                               builder.makeUintConstant(1 << 1));
        source_tile_pixel_x = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, dest_sample_id,
                                                   dest_tile_pixel_x, builder.makeUintConstant(1),
                                                   builder.makeUintConstant(31));
      } else if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
        // 32bpp -> 64bpp, 4x -> 2x.
        // 1 destination horizontal pixel = 2 source horizontal samples.
        // D p0,0 s0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,0 s1 = S p0,0 s0,1 | S p0,0 s1,1
        // D p1,0 s0 = S p1,0 s0,0 | S p1,0 s1,0
        // D p1,0 s1 = S p1,0 s0,1 | S p1,0 s1,1
        // Pixel index can be reused. Sample 1 (for native 2x) or 0 (for 2x as
        // 4x) should become samples 01, sample 0 or 3 should become samples 23.
        if (msaa_2x_attachments_supported_) {
          source_sample_id =
              builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                                  builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                                      builder.makeUintConstant(1)),
                                  builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                                 builder.makeUintConstant(1 << 1));
        }
      } else {
        // 32bpp -> 64bpp, 4x -> 1x.
        // 1 destination horizontal pixel = 2 source horizontal samples.
        // D p0,0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,1 = S p0,0 s0,1 | S p0,0 s1,1
        // Horizontal pixel index can be reused. Vertical pixel 1 should
        // become sample 2.
        source_sample_id = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, builder.makeUintConstant(0), dest_tile_pixel_y,
            builder.makeUintConstant(1), builder.makeUintConstant(1));
        source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_y, builder.makeUintConstant(1));
      }
    } else {
      // 32bpp -> 64bpp, 1x/2x ->.
      // Source has 32bpp halves in two adjacent pixels.
      if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 32bpp -> 64bpp, 1x/2x -> 4x.
        // The X part.
        // 1 destination horizontal sample = 2 source horizontal pixels.
        source_tile_pixel_x = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder.createBinOp(spv::OpShiftLeftLogical, type_uint, dest_tile_pixel_x,
                                builder.makeUintConstant(2)),
            dest_sample_id, builder.makeUintConstant(1), builder.makeUintConstant(1));
        // Y is handled by common code.
      } else {
        // 32bpp -> 64bpp, 1x/2x -> 1x/2x.
        // The X part.
        // 1 destination horizontal pixel = 2 source horizontal pixels.
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(1));
        // Y is handled by common code.
      }
    }
  } else if (source_is_64bpp && !dest_is_64bpp) {
    // 64bpp -> 32bpp, also the half to load.
    if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 64bpp -> 32bpp, -> 4x.
      // The needed half is in the destination horizontal sample index.
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 64bpp -> 32bpp, 4x -> 4x.
        // D p0,0 s0,0 = S s0,0 low
        // D p0,0 s1,0 = S s0,0 high
        // D p1,0 s0,0 = S s1,0 low
        // D p1,0 s1,0 = S s1,0 high
        // Vertical pixel and sample (second bit) addressing is the same.
        // However, 1 horizontal destination pixel = 1 horizontal source sample.
        source_sample_id = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, dest_sample_id,
                                                dest_tile_pixel_x, builder.makeUintConstant(0),
                                                builder.makeUintConstant(1));
        // 2 destination horizontal samples = 1 source horizontal sample, thus
        // 2 destination horizontal pixels = 1 source horizontal pixel.
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(1));
      } else {
        // 64bpp -> 32bpp, 1x/2x -> 4x.
        // 2 destination horizontal samples = 1 source horizontal pixel, thus
        // 1 destination horizontal pixel = 1 source horizontal pixel. Can reuse
        // horizontal pixel index.
        // Y is handled by common code.
      }
      // Half from the destination horizontal sample index.
      source_color_half = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                              builder.makeUintConstant(1));
    } else {
      // 64bpp -> 32bpp, -> 1x/2x.
      // The needed half is in the destination horizontal pixel index.
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 64bpp -> 32bpp, 4x -> 1x/2x.
        // (Destination horizontal pixel >> 1) & 1 = source horizontal sample
        // (first bit).
        source_sample_id =
            builder.createTriOp(spv::OpBitFieldUExtract, type_uint, dest_tile_pixel_x,
                                builder.makeUintConstant(1), builder.makeUintConstant(1));
        if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
          // 64bpp -> 32bpp, 4x -> 2x.
          // Destination vertical samples (1/0 in the first bit for native 2x or
          // 0/1 in the second bit for 2x as 4x) = source vertical samples
          // (second bit).
          if (msaa_2x_attachments_supported_) {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, source_sample_id,
                builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                    builder.makeUintConstant(1)),
                builder.makeUintConstant(1), builder.makeUintConstant(1));
          } else {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_sample_id, source_sample_id,
                builder.makeUintConstant(0), builder.makeUintConstant(1));
          }
        } else {
          // 64bpp -> 32bpp, 4x -> 1x.
          // 1 destination vertical pixel = 1 source vertical sample.
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id, source_tile_pixel_y,
              builder.makeUintConstant(1), builder.makeUintConstant(1));
          source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_y, builder.makeUintConstant(1));
        }
        // 2 destination horizontal pixels = 1 source horizontal sample.
        // 4 destination horizontal pixels = 1 source horizontal pixel.
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(2));
      } else {
        // 64bpp -> 32bpp, 1x/2x -> 1x/2x.
        // The X part.
        // 2 destination horizontal pixels = 1 destination source pixel.
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(1));
        // Y is handled by common code.
      }
      // Half from the destination horizontal pixel index.
      source_color_half = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_x,
                                              builder.makeUintConstant(1));
    }
    assert_true(source_color_half != spv::NoResult);
  } else {
    // Same bit count.
    if (key.source_msaa_samples != key.dest_msaa_samples) {
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // Same BPP, 4x -> 1x/2x.
        if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
          // Same BPP, 4x -> 2x.
          // Horizontal pixels to samples. Vertical sample (1/0 in the first bit
          // for native 2x or 0/1 in the second bit for 2x as 4x) to second
          // sample bit.
          if (msaa_2x_attachments_supported_) {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_tile_pixel_x,
                builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                    builder.makeUintConstant(1)),
                builder.makeUintConstant(1), builder.makeUintConstant(31));
          } else {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                builder.makeUintConstant(0), builder.makeUintConstant(1));
          }
          source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_x, builder.makeUintConstant(1));
        } else {
          // Same BPP, 4x -> 1x.
          // Pixels to samples.
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_x,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(1));
          source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_x, builder.makeUintConstant(1));
          source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_y, builder.makeUintConstant(1));
        }
      } else {
        // Same BPP, 1x/2x -> 1x/2x/4x (as long as they're different).
        // Only the X part - Y is handled by common code.
        if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
          // Horizontal samples to pixels.
          source_tile_pixel_x = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
              builder.makeUintConstant(1), builder.makeUintConstant(31));
        }
      }
    }
  }
  // Common source Y and sample index for 1x/2x AA sources, independent of bits
  // per sample.
  if (key.source_msaa_samples < xenos::MsaaSamples::k4X &&
      key.source_msaa_samples != key.dest_msaa_samples) {
    if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 1x/2x -> 4x.
      if (key.source_msaa_samples == xenos::MsaaSamples::k2X) {
        // 2x -> 4x.
        // Vertical samples (second bit) of 4x destination to vertical sample
        // (1, 0 for native 2x, or 0, 3 for 2x as 4x) of 2x source.
        source_sample_id = builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                               builder.makeUintConstant(1));
        if (msaa_2x_attachments_supported_) {
          source_sample_id = builder.createBinOp(spv::OpBitwiseXor, type_uint, source_sample_id,
                                                 builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id, source_sample_id,
              builder.makeUintConstant(1), builder.makeUintConstant(1));
        }
      } else {
        // 1x -> 4x.
        // Vertical samples (second bit) to Y pixels.
        source_tile_pixel_y = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                builder.makeUintConstant(1)),
            dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
      }
    } else {
      // 1x/2x -> different 1x/2x.
      if (key.source_msaa_samples == xenos::MsaaSamples::k2X) {
        // 2x -> 1x.
        // Vertical pixels of 2x destination to vertical samples (1, 0 for
        // native 2x, or 0, 3 for 2x as 4x) of 1x source.
        source_sample_id = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_y,
                                               builder.makeUintConstant(1));
        if (msaa_2x_attachments_supported_) {
          source_sample_id = builder.createBinOp(spv::OpBitwiseXor, type_uint, source_sample_id,
                                                 builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id, source_sample_id,
              builder.makeUintConstant(1), builder.makeUintConstant(1));
        }
        source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_y, builder.makeUintConstant(1));
      } else {
        // 1x -> 2x.
        // Vertical samples (1/0 in the first bit for native 2x or 0/1 in the
        // second bit for 2x as 4x) of 2x destination to vertical pixels of 1x
        // source.
        if (msaa_2x_attachments_supported_) {
          source_tile_pixel_y = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
        } else {
          source_tile_pixel_y = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
        }
      }
    }
  }

  uint32_t source_pixel_width_dwords_log2 =
      uint32_t(key.source_msaa_samples >= xenos::MsaaSamples::k4X) + uint32_t(source_is_64bpp);

  if (source_is_color != dest_is_color) {
    // Copying between color and depth / stencil - swap 40-32bpp-sample columns
    // in the pixel index within the source 32bpp tile.
    uint32_t source_32bpp_tile_half_pixels =
        tile_width_samples >> (1 + source_pixel_width_dwords_log2);
    source_tile_pixel_x = builder.createUnaryOp(
        spv::OpBitcast, type_uint,
        builder.createBinOp(
            spv::OpIAdd, type_int,
            builder.createUnaryOp(spv::OpBitcast, type_int, source_tile_pixel_x),
            builder.createTriOp(
                spv::OpSelect, type_int,
                builder.createBinOp(spv::OpULessThan, builder.makeBoolType(), source_tile_pixel_x,
                                    builder.makeUintConstant(source_32bpp_tile_half_pixels)),
                builder.makeIntConstant(int32_t(source_32bpp_tile_half_pixels)),
                builder.makeIntConstant(-int32_t(source_32bpp_tile_half_pixels)))));
  }

  // Transform the destination 32bpp tile index into the source. After the
  // addition, it may be negative - in which case, the transfer is done across
  // EDRAM addressing wrapping, and xenos::kEdramTileCount must be added to it,
  // but `& (xenos::kEdramTileCount - 1)` handles that regardless of the sign.
  spv::Id source_tile_index = builder.createBinOp(
      spv::OpBitwiseAnd, type_uint,
      builder.createUnaryOp(
          spv::OpBitcast, type_uint,
          builder.createBinOp(
              spv::OpIAdd, type_int,
              builder.createUnaryOp(spv::OpBitcast, type_int, dest_tile_index),
              builder.createTriOp(spv::OpBitFieldSExtract, type_int,
                                  builder.createUnaryOp(spv::OpBitcast, type_int, address_constant),
                                  builder.makeUintConstant(xenos::kEdramPitchTilesBits * 2),
                                  builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1)))),
      builder.makeUintConstant(xenos::kEdramTileCount - 1));
  // Split the source 32bpp tile index into X and Y tile index within the source
  // image.
  spv::Id source_pitch_tiles =
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, address_constant,
                          builder.makeUintConstant(xenos::kEdramPitchTilesBits),
                          builder.makeUintConstant(xenos::kEdramPitchTilesBits));
  spv::Id source_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, source_tile_index, source_pitch_tiles);
  spv::Id source_tile_index_x =
      builder.createBinOp(spv::OpUMod, type_uint, source_tile_index, source_pitch_tiles);
  // Finally calculate the source texture coordinates.
  spv::Id source_pixel_x_int = builder.createUnaryOp(
      spv::OpBitcast, type_int,
      builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(
              spv::OpIMul, type_uint,
              builder.makeUintConstant(tile_width_samples >> source_pixel_width_dwords_log2),
              source_tile_index_x),
          source_tile_pixel_x));
  spv::Id source_pixel_y_int = builder.createUnaryOp(
      spv::OpBitcast, type_int,
      builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(
              spv::OpIMul, type_uint,
              builder.makeUintConstant(tile_height_samples >> uint32_t(key.source_msaa_samples >=
                                                                       xenos::MsaaSamples::k2X)),
              source_tile_index_y),
          source_tile_pixel_y));

  // Load the source.

  spv::Builder::TextureParameters source_texture_parameters = {};
  id_vector_temp.clear();
  id_vector_temp.push_back(source_pixel_x_int);
  id_vector_temp.push_back(source_pixel_y_int);
  spv::Id source_coordinates[2] = {
      builder.createCompositeConstruct(type_int2, id_vector_temp),
  };
  spv::Id source_sample_ids_int[2] = {};
  if (key.source_msaa_samples != xenos::MsaaSamples::k1X) {
    source_sample_ids_int[0] = builder.createUnaryOp(spv::OpBitcast, type_int, source_sample_id);
  } else {
    source_texture_parameters.lod = builder.makeIntConstant(0);
  }
  // Go to the next sample or pixel along X if need to load two dwords.
  bool source_load_is_two_32bpp_samples = !source_is_64bpp && dest_is_64bpp;
  if (source_load_is_two_32bpp_samples) {
    if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
      source_coordinates[1] = source_coordinates[0];
      source_sample_ids_int[1] = builder.createBinOp(
          spv::OpBitwiseOr, type_int, source_sample_ids_int[0], builder.makeIntConstant(1));
    } else {
      id_vector_temp.clear();
      id_vector_temp.push_back(builder.createBinOp(spv::OpBitwiseOr, type_int, source_pixel_x_int,
                                                   builder.makeIntConstant(1)));
      id_vector_temp.push_back(source_pixel_y_int);
      source_coordinates[1] = builder.createCompositeConstruct(type_int2, id_vector_temp);
      source_sample_ids_int[1] = source_sample_ids_int[0];
    }
  }
  spv::Id source_color[2][4] = {};
  if (source_color_texture != spv::NoResult) {
    source_texture_parameters.sampler = builder.createLoad(source_color_texture, spv::NoPrecision);
    assert_true(source_color_component_type != spv::NoType);
    spv::Id source_color_vec4_type = builder.makeVectorType(source_color_component_type, 4);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      spv::Id source_color_vec4 = builder.createTextureCall(
          spv::NoPrecision, source_color_vec4_type, false, true, false, false, false,
          source_texture_parameters, spv::ImageOperandsMaskNone);
      uint32_t source_color_components_remaining = source_color_texture_component_mask;
      uint32_t source_color_component_index;
      while (
          rex::bit_scan_forward(source_color_components_remaining, &source_color_component_index)) {
        source_color_components_remaining &= ~(uint32_t(1) << source_color_component_index);
        source_color[i][source_color_component_index] = builder.createCompositeExtract(
            source_color_vec4, source_color_component_type, source_color_component_index);
      }
    }
  }
  spv::Id source_depth_float[2] = {};
  if (source_depth_texture != spv::NoResult) {
    source_texture_parameters.sampler = builder.createLoad(source_depth_texture, spv::NoPrecision);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      source_depth_float[i] = builder.createCompositeExtract(
          builder.createTextureCall(spv::NoPrecision, type_float4, false, true, false, false, false,
                                    source_texture_parameters, spv::ImageOperandsMaskNone),
          type_float, 0);
    }
  }
  spv::Id source_stencil[2] = {};
  if (source_stencil_texture != spv::NoResult) {
    source_texture_parameters.sampler =
        builder.createLoad(source_stencil_texture, spv::NoPrecision);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      source_stencil[i] = builder.createCompositeExtract(
          builder.createTextureCall(spv::NoPrecision, type_uint4, false, true, false, false, false,
                                    source_texture_parameters, spv::ImageOperandsMaskNone),
          type_uint, 0);
    }
  }

  // Pick the needed 32bpp half of the 64bpp color.
  if (source_is_64bpp && !dest_is_64bpp) {
    uint32_t source_color_half_component_count = source_color_format_component_count >> 1;
    assert_true(source_color_half != spv::NoResult);
    spv::Id source_color_is_second_half = builder.createBinOp(
        spv::OpINotEqual, type_bool, source_color_half, builder.makeUintConstant(0));
    if (mode.output == TransferOutput::kStencilBit) {
      source_color[0][0] = builder.createTriOp(
          spv::OpSelect, source_color_component_type, source_color_is_second_half,
          source_color[0][source_color_half_component_count], source_color[0][0]);
    } else {
      for (uint32_t i = 0; i < source_color_half_component_count; ++i) {
        source_color[0][i] = builder.createTriOp(
            spv::OpSelect, source_color_component_type, source_color_is_second_half,
            source_color[0][source_color_half_component_count + i], source_color[0][i]);
      }
    }
  }

  if (output_fragment_stencil_ref != spv::NoResult && source_stencil[0] != spv::NoResult) {
    // For the depth -> depth case, write the stencil directly to the output.
    assert_true(mode.output == TransferOutput::kDepth);
    builder.createStore(builder.createUnaryOp(spv::OpBitcast, type_int, source_stencil[0]),
                        output_fragment_stencil_ref);
  }

  const bool source_color_16_is_float = IsColor16FormatFloatLike(source_color_format);
  const bool dest_color_16_is_float = IsColor16FormatFloatLike(dest_color_format);
  spv::Id const_uint_0 = builder.makeUintConstant(0);
  spv::Id const_uint_16 = builder.makeUintConstant(16);
  spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
  spv::Id const_float_1 = builder.makeFloatConstant(1.0f);
  spv::Id const_float_minus_1 = builder.makeFloatConstant(-1.0f);
  spv::Id const_float_32767 = builder.makeFloatConstant(32767.0f);
  spv::Id const_float_inv_32767 = builder.makeFloatConstant(1.0f / 32767.0f);
  auto PWLGammaToLinear = [&](spv::Id gamma, bool gamma_pre_saturated) -> spv::Id {
    if (!gamma_pre_saturated) {
      gamma = builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                           gamma, const_float_0, const_float_1);
    }
    spv::Id is_piece_at_least_3 = builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, gamma,
                                                      builder.makeFloatConstant(192.0f / 255.0f));
    spv::Id scale_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                               builder.makeFloatConstant(8.0f / 1024.0f),
                                               builder.makeFloatConstant(4.0f / 1024.0f));
    spv::Id offset_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                                builder.makeFloatConstant(-1024.0f),
                                                builder.makeFloatConstant(-256.0f));
    spv::Id is_piece_at_least_1 = builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, gamma,
                                                      builder.makeFloatConstant(64.0f / 255.0f));
    spv::Id scale_1_or_0 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                                               builder.makeFloatConstant(2.0f / 1024.0f),
                                               builder.makeFloatConstant(1.0f / 1024.0f));
    spv::Id offset_1_or_0 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                                                builder.makeFloatConstant(-64.0f), const_float_0);
    spv::Id is_piece_at_least_2 = builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, gamma,
                                                      builder.makeFloatConstant(96.0f / 255.0f));
    spv::Id scale = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                        scale_3_or_2, scale_1_or_0);
    spv::Id offset = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                         offset_3_or_2, offset_1_or_0);
    spv::Id linear = builder.createBinOp(
        spv::OpFAdd, type_float,
        builder.createBinOp(spv::OpFMul, type_float,
                            builder.createBinOp(spv::OpFMul, type_float, gamma,
                                                builder.makeFloatConstant(255.0f * 1024.0f)),
                            scale),
        offset);
    linear = builder.createBinOp(spv::OpFAdd, type_float, linear,
                                 builder.createUnaryBuiltinCall(
                                     type_float, ext_inst_glsl_std_450, GLSLstd450Trunc,
                                     builder.createBinOp(spv::OpFMul, type_float, linear, scale)));
    return builder.createBinOp(spv::OpFMul, type_float, linear,
                               builder.makeFloatConstant(1.0f / 1023.0f));
  };
  auto LinearToPWLGamma = [&](spv::Id linear, bool linear_pre_saturated) -> spv::Id {
    if (!linear_pre_saturated) {
      linear = builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                            linear, const_float_0, const_float_1);
    }
    spv::Id is_piece_at_least_3 =
        builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, linear,
                            builder.makeFloatConstant(512.0f / 1023.0f));
    spv::Id scale_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                               builder.makeFloatConstant(1023.0f / 8.0f),
                                               builder.makeFloatConstant(1023.0f / 4.0f));
    spv::Id offset_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                                builder.makeFloatConstant(128.0f / 255.0f),
                                                builder.makeFloatConstant(64.0f / 255.0f));
    spv::Id is_piece_at_least_1 = builder.createBinOp(
        spv::OpFOrdGreaterThanEqual, type_bool, linear, builder.makeFloatConstant(64.0f / 1023.0f));
    spv::Id scale_1_or_0 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                                               builder.makeFloatConstant(1023.0f / 2.0f),
                                               builder.makeFloatConstant(1023.0f));
    spv::Id offset_1_or_0 =
        builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                            builder.makeFloatConstant(32.0f / 255.0f), const_float_0);
    spv::Id is_piece_at_least_2 =
        builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, linear,
                            builder.makeFloatConstant(128.0f / 1023.0f));
    spv::Id scale = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                        scale_3_or_2, scale_1_or_0);
    spv::Id offset = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                         offset_3_or_2, offset_1_or_0);
    return builder.createBinOp(
        spv::OpFAdd, type_float,
        builder.createBinOp(spv::OpFMul, type_float,
                            builder.createUnaryBuiltinCall(
                                type_float, ext_inst_glsl_std_450, GLSLstd450Trunc,
                                builder.createBinOp(spv::OpFMul, type_float, linear, scale)),
                            builder.makeFloatConstant(1.0f / 255.0f)),
        offset);
  };
  auto PackSource16ComponentToUint = [&](spv::Id component) -> spv::Id {
    if (source_color_is_uint) {
      return component;
    }
    if (source_color_16_is_float) {
      id_vector_temp.clear();
      id_vector_temp.push_back(component);
      id_vector_temp.push_back(const_float_0);
      spv::Id packed_half = builder.createUnaryBuiltinCall(
          type_uint, ext_inst_glsl_std_450, GLSLstd450PackHalf2x16,
          builder.createCompositeConstruct(type_float2, id_vector_temp));
      return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed_half, const_uint_0,
                                 const_uint_16);
    }
    spv::Id component_clamped =
        builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp, component,
                                     const_float_minus_1, const_float_1);
    spv::Id component_rounded = builder.createUnaryBuiltinCall(
        type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
        builder.createBinOp(spv::OpFMul, type_float, component_clamped, const_float_32767));
    spv::Id component_snorm =
        builder.createUnaryOp(spv::OpConvertFToS, type_int, component_rounded);
    return builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                               builder.createUnaryOp(spv::OpBitcast, type_uint, component_snorm),
                               const_uint_0, const_uint_16);
  };
  auto PackSource16PairToUint32 = [&](spv::Id component_0, spv::Id component_1) -> spv::Id {
    return builder.createQuadOp(
        spv::OpBitFieldInsert, type_uint, PackSource16ComponentToUint(component_0),
        PackSource16ComponentToUint(component_1), const_uint_16, const_uint_16);
  };
  auto UnpackDest16ComponentFromUint32 = [&](spv::Id packed_word,
                                             uint32_t component_index) -> spv::Id {
    spv::Id component_offset = component_index & 1 ? const_uint_16 : const_uint_0;
    if (dest_color_is_uint) {
      return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed_word, component_offset,
                                 const_uint_16);
    }
    if (dest_color_16_is_float) {
      spv::Id component_pair = builder.createUnaryBuiltinCall(
          type_float2, ext_inst_glsl_std_450, GLSLstd450UnpackHalf2x16, packed_word);
      return builder.createCompositeExtract(component_pair, type_float, component_index & 1);
    }
    spv::Id component_snorm = builder.createTriOp(spv::OpBitFieldSExtract, type_int, packed_word,
                                                  component_offset, const_uint_16);
    spv::Id component_float =
        builder.createBinOp(spv::OpFMul, type_float,
                            builder.createUnaryOp(spv::OpConvertSToF, type_float, component_snorm),
                            const_float_inv_32767);
    return builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                        component_float, const_float_minus_1, const_float_1);
  };

  if (dest_is_64bpp) {
    // Construct the 64bpp color from two 32-bit samples or one 64-bit sample.
    // If `packed` (two uints) are created, use the generic path involving
    // unpacking.
    // Otherwise, the fragment data output must be written to directly by the
    // reached control flow path.
    spv::Id packed[2] = {};
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (gamma_render_target_as_unorm16_) {
            // 8_8_8_8_GAMMA is represented by linear stored in
            // R16G16B16A16_UNORM.
            for (uint32_t i = 0; i < 2; ++i) {
              for (uint32_t j = 0; j < 3; ++j) {
                source_color[i][j] = LinearToPWLGamma(source_color[i][j], true);
              }
            }
          }
        }
          [[fallthrough]];
        case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
          spv::Id component_width = builder.makeUintConstant(8);
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(spv::OpFMul, type_float, source_color[i][0], unorm_scale),
                    unorm_round_offset));
            for (uint32_t j = 1; j < 4; ++j) {
              packed[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed[i],
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(spv::OpFAdd, type_float,
                                          builder.createBinOp(spv::OpFMul, type_float,
                                                              source_color[i][j], unorm_scale),
                                          unorm_round_offset)),
                  builder.makeUintConstant(8 * j), component_width);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
          spv::Id width_rgb = builder.makeUintConstant(10);
          spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
          spv::Id width_a = builder.makeUintConstant(2);
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[i][0],
                                                        unorm_scale_rgb),
                                    unorm_round_offset));
            for (uint32_t j = 1; j < 4; ++j) {
              packed[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed[i],
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float, source_color[i][j],
                                              j == 3 ? unorm_scale_a : unorm_scale_rgb),
                          unorm_round_offset)),
                  builder.makeUintConstant(10 * j), j == 3 ? width_a : width_rgb);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          spv::Id width_rgb = builder.makeUintConstant(10);
          spv::Id float_0 = builder.makeFloatConstant(0.0f);
          spv::Id float_1 = builder.makeFloatConstant(1.0f);
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
          spv::Id offset_a = builder.makeUintConstant(30);
          spv::Id width_a = builder.makeUintConstant(2);
          for (uint32_t i = 0; i < 2; ++i) {
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            packed[i] = SpirvShaderTranslator::UnclampedFloat32To7e3(builder, source_color[i][0],
                                                                     ext_inst_glsl_std_450);
            for (uint32_t j = 1; j < 3; ++j) {
              packed[i] =
                  builder.createQuadOp(spv::OpBitFieldInsert, type_uint, packed[i],
                                       SpirvShaderTranslator::UnclampedFloat32To7e3(
                                           builder, source_color[i][j], ext_inst_glsl_std_450),
                                       builder.makeUintConstant(10 * j), width_rgb);
            }
            // Saturate and convert the alpha.
            spv::Id alpha_saturated =
                builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                             source_color[i][3], float_0, float_1);
            packed[i] = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, packed[i],
                builder.createUnaryOp(
                    spv::OpConvertFToU, type_uint,
                    builder.createBinOp(spv::OpFAdd, type_float,
                                        builder.createBinOp(spv::OpFMul, type_float,
                                                            alpha_saturated, unorm_scale_a),
                                        unorm_round_offset)),
                offset_a, width_a);
          }
        } break;
        // Route through packed uint32 words so mixed source/destination
        // transfer component types (integer vs float fallback) are handled
        // uniformly.
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = PackSource16PairToUint32(source_color[i][0], source_color[i][1]);
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] =
                PackSource16PairToUint32(source_color[0][i << 1], source_color[0][(i << 1) + 1]);
          }
        } break;
        // Float32 is transferred as uint32 to preserve NaN encodings. However,
        // multisampled sampled image support is optional in Vulkan.
        case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = source_color[i][0];
            if (!source_color_is_uint) {
              packed[i] = builder.createUnaryOp(spv::OpBitcast, type_uint, packed[i]);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = source_color[0][i];
            if (!source_color_is_uint) {
              packed[i] = builder.createUnaryOp(spv::OpBitcast, type_uint, packed[i]);
            }
          }
        } break;
      }
    } else {
      assert_true(source_depth_texture != spv::NoResult);
      assert_true(source_stencil_texture != spv::NoResult);
      spv::Id depth_offset = builder.makeUintConstant(8);
      spv::Id depth_width = builder.makeUintConstant(24);
      for (uint32_t i = 0; i < 2; ++i) {
        spv::Id depth24 = spv::NoResult;
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to the nearest even integer. This seems to be the
            // correct conversion, adding +0.5 and rounding towards zero results
            // in red instead of black in the 4D5307E6 clear shader.
            depth24 = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createUnaryBuiltinCall(
                    type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                    builder.createBinOp(spv::OpFMul, type_float, source_depth_float[i],
                                        builder.makeFloatConstant(float(0xFFFFFF)))));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            depth24 = SpirvShaderTranslator::PreClampedDepthTo20e4(
                builder, source_depth_float[i], depth_float24_round(), true, ext_inst_glsl_std_450);
          } break;
        }
        // Merge depth and stencil.
        packed[i] = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, source_stencil[i],
                                         depth24, depth_offset, depth_width);
      }
    }
    // Common path unless there was a specialized one - unpack two packed 32-bit
    // parts.
    if (packed[0] != spv::NoResult) {
      assert_true(packed[1] != spv::NoResult);
      if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
        id_vector_temp.clear();
        id_vector_temp.push_back(packed[0]);
        id_vector_temp.push_back(packed[1]);
        // If integer transfer formats are unavailable for this sample count,
        // ownership transfer falls back to float formats and raw bits are
        // passed via bitcasts.
        if (!dest_color_is_uint) {
          for (spv::Id& float32 : id_vector_temp) {
            float32 = builder.createUnaryOp(spv::OpBitcast, type_float, float32);
          }
        }
        builder.createStore(builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                            output_fragment_data);
      } else {
        id_vector_temp.clear();
        for (uint32_t i = 0; i < 4; ++i) {
          id_vector_temp.push_back(UnpackDest16ComponentFromUint32(packed[i >> 1], i));
        }
        builder.createStore(builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                            output_fragment_data);
      }
    }
  } else {
    // If `packed` is created, use the generic path involving unpacking.
    // - For a color destination, the packed 32bpp color.
    // - For a depth / stencil destination, stencil in 0:7, depth in 8:31
    //   normally, or depth in 0:23 and zeros in 24:31 with packed_only_depth.
    // - For a stencil bit, stencil in 0:7.
    // Otherwise, the fragment data or fragment depth / stencil output must be
    // written to directly by the reached control flow path.
    spv::Id packed = spv::NoResult;
    bool packed_only_depth = false;
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (mode.output == TransferOutput::kStencilBit) {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                gamma_render_target_as_unorm16_) {
              source_color[0][0] = LinearToPWLGamma(source_color[0][0], true);
            }
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][0],
                                                        builder.makeFloatConstant(255.0f)),
                                    builder.makeFloatConstant(0.5f)));
          } else if (dest_is_color &&
                     (dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
                      dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)) {
            if (source_color_format != dest_color_format) {
              // Color space conversion between k_8_8_8_8 and
              // k_8_8_8_8_GAMMA.
              if (dest_color_format != xenos::ColorRenderTargetFormat::k_8_8_8_8) {
                for (uint32_t i = 0; i < 3; ++i) {
                  source_color[0][i] = LinearToPWLGamma(source_color[0][i], true);
                }
              } else {
                for (uint32_t i = 0; i < 3; ++i) {
                  source_color[0][i] = PWLGammaToLinear(source_color[0][i], true);
                }
              }
            }
            // Same or converted format - passthrough.
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(
                builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                output_fragment_data);
          } else if (mode.output == TransferOutput::kDepth) {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                gamma_render_target_as_unorm16_) {
              for (uint32_t i = output_fragment_stencil_ref != spv::NoResult ? 0 : 1; i < 3; ++i) {
                source_color[0][i] = LinearToPWLGamma(source_color[0][i], true);
              }
            }
            // When need only depth, not stencil, skip the red component.
            packed_only_depth = true;
            if (output_fragment_stencil_ref != spv::NoResult) {
              // Write the red component to the stencil reference.
              builder.createStore(
                  builder.createUnaryOp(
                      spv::OpBitcast, type_int,
                      builder.createUnaryOp(
                          spv::OpConvertFToU, type_uint,
                          builder.createBinOp(
                              spv::OpFAdd, type_float,
                              builder.createBinOp(spv::OpFMul, type_float, source_color[0][0],
                                                  builder.makeFloatConstant(255.0f)),
                              builder.makeFloatConstant(0.5f)))),
                  output_fragment_stencil_ref);
            }
            // Put depth in 0:23.
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][1],
                                                        builder.makeFloatConstant(255.0f)),
                                    builder.makeFloatConstant(0.5f)));
            spv::Id component_width = builder.makeUintConstant(8);
            for (uint32_t i = 2; i < 4; ++i) {
              packed = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed,
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float, source_color[0][i],
                                              builder.makeFloatConstant(255.0f)),
                          builder.makeFloatConstant(0.5f))),
                  builder.makeUintConstant(8 * (i - 1)), component_width);
            }
          } else {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                gamma_render_target_as_unorm16_) {
              for (uint32_t i = 0; i < 3; ++i) {
                source_color[0][i] = LinearToPWLGamma(source_color[0][i], true);
              }
            }
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][0],
                                                        builder.makeFloatConstant(255.0f)),
                                    builder.makeFloatConstant(0.5f)));
            spv::Id component_width = builder.makeUintConstant(8);
            for (uint32_t i = 1; i < 4; ++i) {
              packed = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed,
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float, source_color[0][i],
                                              builder.makeFloatConstant(255.0f)),
                          builder.makeFloatConstant(0.5f))),
                  builder.makeUintConstant(8 * i), component_width);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          if (dest_is_color &&
              (dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
               dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10)) {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(
                builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                output_fragment_data);
          } else {
            spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
            spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][0],
                                                        unorm_scale_rgb),
                                    unorm_round_offset));
            if (mode.output != TransferOutput::kStencilBit) {
              spv::Id width_rgb = builder.makeUintConstant(10);
              spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
              spv::Id width_a = builder.makeUintConstant(2);
              for (uint32_t i = 1; i < 4; ++i) {
                packed = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, packed,
                    builder.createUnaryOp(
                        spv::OpConvertFToU, type_uint,
                        builder.createBinOp(
                            spv::OpFAdd, type_float,
                            builder.createBinOp(spv::OpFMul, type_float, source_color[0][i],
                                                i == 3 ? unorm_scale_a : unorm_scale_rgb),
                            unorm_round_offset)),
                    builder.makeUintConstant(10 * i), i == 3 ? width_a : width_rgb);
              }
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          if (dest_is_color &&
              (dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
               dest_color_format ==
                   xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16)) {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(
                builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                output_fragment_data);
          } else {
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            packed = SpirvShaderTranslator::UnclampedFloat32To7e3(builder, source_color[0][0],
                                                                  ext_inst_glsl_std_450);
            if (mode.output != TransferOutput::kStencilBit) {
              spv::Id width_rgb = builder.makeUintConstant(10);
              for (uint32_t i = 1; i < 3; ++i) {
                packed =
                    builder.createQuadOp(spv::OpBitFieldInsert, type_uint, packed,
                                         SpirvShaderTranslator::UnclampedFloat32To7e3(
                                             builder, source_color[0][i], ext_inst_glsl_std_450),
                                         builder.makeUintConstant(10 * i), width_rgb);
              }
              // Saturate and convert the alpha.
              spv::Id alpha_saturated = builder.createTriBuiltinCall(
                  type_float, ext_inst_glsl_std_450, GLSLstd450NClamp, source_color[0][3],
                  builder.makeFloatConstant(0.0f), builder.makeFloatConstant(1.0f));
              packed = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed,
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float, alpha_saturated,
                                              builder.makeFloatConstant(3.0f)),
                          builder.makeFloatConstant(0.5f))),
                  builder.makeUintConstant(30), builder.makeUintConstant(2));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          // Route through packed uint32 so destination transfer type (integer
          // vs float fallback) is reconstructed consistently.
          packed = PackSource16ComponentToUint(source_color[0][0]);
          if (mode.output != TransferOutput::kStencilBit) {
            packed = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, packed,
                                          PackSource16ComponentToUint(source_color[0][1]),
                                          const_uint_16, const_uint_16);
          }
        } break;
        // Float32 is transferred as uint32 to preserve NaN encodings. However,
        // multisampled sampled image support is optional in Vulkan.
        case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          packed = source_color[0][0];
          if (!source_color_is_uint) {
            packed = builder.createUnaryOp(spv::OpBitcast, type_uint, packed);
          }
        } break;
      }
    } else if (source_depth_float[0] != spv::NoResult) {
      if (mode.output == TransferOutput::kDepth && dest_depth_format == source_depth_format) {
        builder.createStore(source_depth_float[0], output_fragment_depth);
      } else {
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to the nearest even integer. This seems to be the correct
            // conversion, adding +0.5 and rounding towards zero results in red
            // instead of black in the 4D5307E6 clear shader.
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createUnaryBuiltinCall(
                    type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                    builder.createBinOp(spv::OpFMul, type_float, source_depth_float[0],
                                        builder.makeFloatConstant(float(0xFFFFFF)))));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            packed = SpirvShaderTranslator::PreClampedDepthTo20e4(
                builder, source_depth_float[0], depth_float24_round(), true, ext_inst_glsl_std_450);
          } break;
        }
        if (mode.output == TransferOutput::kDepth) {
          packed_only_depth = true;
        } else {
          // Merge depth and stencil.
          packed = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, source_stencil[0], packed,
                                        builder.makeUintConstant(8), builder.makeUintConstant(24));
        }
      }
    }
    switch (mode.output) {
      case TransferOutput::kColor: {
        // Unless a special path was taken, unpack the raw 32bpp value into the
        // 32bpp color output.
        if (packed != spv::NoResult) {
          switch (dest_color_format) {
            case xenos::ColorRenderTargetFormat::k_8_8_8_8:
            case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
              spv::Id component_width = builder.makeUintConstant(8);
              spv::Id unorm_scale = builder.makeFloatConstant(1.0f / 255.0f);
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 4; ++i) {
                id_vector_temp.push_back(builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createUnaryOp(
                        spv::OpConvertUToF, type_float,
                        builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed,
                                            builder.makeUintConstant(8 * i), component_width)),
                    unorm_scale));
              }
              if (dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                  gamma_render_target_as_unorm16_) {
                // 8_8_8_8_GAMMA is represented by linear stored in
                // R16G16B16A16_UNORM.
                for (uint32_t i = 0; i < 3; ++i) {
                  id_vector_temp[i] = PWLGammaToLinear(id_vector_temp[i], true);
                }
              }
              builder.createStore(
                  builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
              spv::Id width_rgb = builder.makeUintConstant(10);
              spv::Id unorm_scale_rgb = builder.makeFloatConstant(1.0f / 1023.0f);
              spv::Id width_a = builder.makeUintConstant(2);
              spv::Id unorm_scale_a = builder.makeFloatConstant(1.0f / 3.0f);
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 4; ++i) {
                id_vector_temp.push_back(builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createUnaryOp(
                        spv::OpConvertUToF, type_float,
                        builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed,
                                            builder.makeUintConstant(10 * i),
                                            i == 3 ? width_a : width_rgb)),
                    i == 3 ? unorm_scale_a : unorm_scale_rgb));
              }
              builder.createStore(
                  builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
              id_vector_temp.clear();
              // Color.
              for (uint32_t i = 0; i < 3; ++i) {
                id_vector_temp.push_back(SpirvShaderTranslator::Float7e3To32(
                    builder, packed, 10 * i, false, ext_inst_glsl_std_450));
              }
              // Alpha.
              id_vector_temp.push_back(builder.createBinOp(
                  spv::OpFMul, type_float,
                  builder.createUnaryOp(spv::OpConvertUToF, type_float,
                                        builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                                                            packed, builder.makeUintConstant(30),
                                                            builder.makeUintConstant(2))),
                  builder.makeFloatConstant(1.0f / 3.0f)));
              builder.createStore(
                  builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 2; ++i) {
                id_vector_temp.push_back(UnpackDest16ComponentFromUint32(packed, i));
              }
              builder.createStore(
                  builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
              // Float32 is transferred as uint32 to preserve NaN encodings.
              // If the integer transfer format is unavailable for this sample
              // count, the transfer format is float and this bitcast path is
              // used.
              spv::Id float32 = packed;
              if (!dest_color_is_uint) {
                float32 = builder.createUnaryOp(spv::OpBitcast, type_float, float32);
              }
              builder.createStore(float32, output_fragment_data);
            } break;
            default:
              // A 64bpp format (handled separately) or an invalid one.
              assert_unhandled_case(dest_color_format);
          }
        }
      } break;
      case TransferOutput::kDepth: {
        if (packed) {
          spv::Id guest_depth24 = packed;
          if (!packed_only_depth) {
            // Extract the depth bits.
            guest_depth24 = builder.createBinOp(spv::OpShiftRightLogical, type_uint, guest_depth24,
                                                builder.makeUintConstant(8));
          }
          // Load the host float32 depth, check if, when converted to the guest
          // format, it's the same as the guest source, thus up to date, and if
          // it is, write host float32 depth, otherwise do the guest -> host
          // conversion.
          spv::Id host_depth32 = spv::NoResult;
          if (host_depth_source_texture != spv::NoResult) {
            // Convert position and sample index from within the destination
            // tile to within the host depth source tile, like for the guest
            // render target, but for 32bpp -> 32bpp only.
            spv::Id host_depth_source_sample_id = dest_sample_id;
            spv::Id host_depth_source_tile_pixel_x = dest_tile_pixel_x;
            spv::Id host_depth_source_tile_pixel_y = dest_tile_pixel_y;
            if (key.host_depth_source_msaa_samples != key.dest_msaa_samples) {
              if (key.host_depth_source_msaa_samples >= xenos::MsaaSamples::k4X) {
                // 4x -> 1x/2x.
                if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
                  // 4x -> 2x.
                  // Horizontal pixels to samples. Vertical sample (1/0 in the
                  // first bit for native 2x or 0/1 in the second bit for 2x as
                  // 4x) to second sample bit.
                  if (msaa_2x_attachments_supported_) {
                    host_depth_source_sample_id = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint, dest_tile_pixel_x,
                        builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                            builder.makeUintConstant(1)),
                        builder.makeUintConstant(1), builder.makeUintConstant(31));
                  } else {
                    host_depth_source_sample_id = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                        builder.makeUintConstant(0), builder.makeUintConstant(1));
                  }
                  host_depth_source_tile_pixel_x =
                      builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_x,
                                          builder.makeUintConstant(1));
                } else {
                  // 4x -> 1x.
                  // Pixels to samples.
                  host_depth_source_sample_id = builder.createQuadOp(
                      spv::OpBitFieldInsert, type_uint,
                      builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_x,
                                          builder.makeUintConstant(1)),
                      dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(1));
                  host_depth_source_tile_pixel_x =
                      builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_x,
                                          builder.makeUintConstant(1));
                  host_depth_source_tile_pixel_y =
                      builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_y,
                                          builder.makeUintConstant(1));
                }
              } else {
                // 1x/2x -> 1x/2x/4x (as long as they're different).
                // Only the X part - Y is handled by common code.
                if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                  // Horizontal samples to pixels.
                  host_depth_source_tile_pixel_x = builder.createQuadOp(
                      spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                      builder.makeUintConstant(1), builder.makeUintConstant(31));
                }
              }
              // Host depth source Y and sample index for 1x/2x AA sources.
              if (key.host_depth_source_msaa_samples < xenos::MsaaSamples::k4X) {
                if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                  // 1x/2x -> 4x.
                  if (key.host_depth_source_msaa_samples == xenos::MsaaSamples::k2X) {
                    // 2x -> 4x.
                    // Vertical samples (second bit) of 4x destination to
                    // vertical sample (1, 0 for native 2x, or 0, 3 for 2x as
                    // 4x) of 2x source.
                    host_depth_source_sample_id =
                        builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                            builder.makeUintConstant(1));
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_sample_id = builder.createBinOp(
                          spv::OpBitwiseXor, type_uint, host_depth_source_sample_id,
                          builder.makeUintConstant(1));
                    } else {
                      host_depth_source_sample_id = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint, host_depth_source_sample_id,
                          host_depth_source_sample_id, builder.makeUintConstant(1),
                          builder.makeUintConstant(1));
                    }
                  } else {
                    // 1x -> 4x.
                    // Vertical samples (second bit) to Y pixels.
                    host_depth_source_tile_pixel_y = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint,
                        builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                            builder.makeUintConstant(1)),
                        dest_tile_pixel_y, builder.makeUintConstant(1),
                        builder.makeUintConstant(31));
                  }
                } else {
                  // 1x/2x -> different 1x/2x.
                  if (key.host_depth_source_msaa_samples == xenos::MsaaSamples::k2X) {
                    // 2x -> 1x.
                    // Vertical pixels of 2x destination to vertical samples (1,
                    // 0 for native 2x, or 0, 3 for 2x as 4x) of 1x source.
                    host_depth_source_sample_id =
                        builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_y,
                                            builder.makeUintConstant(1));
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_sample_id = builder.createBinOp(
                          spv::OpBitwiseXor, type_uint, host_depth_source_sample_id,
                          builder.makeUintConstant(1));
                    } else {
                      host_depth_source_sample_id = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint, host_depth_source_sample_id,
                          host_depth_source_sample_id, builder.makeUintConstant(1),
                          builder.makeUintConstant(1));
                    }
                    host_depth_source_tile_pixel_y =
                        builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_y,
                                            builder.makeUintConstant(1));
                  } else {
                    // 1x -> 2x.
                    // Vertical samples (1/0 in the first bit for native 2x or
                    // 0/1 in the second bit for 2x as 4x) of 2x destination to
                    // vertical pixels of 1x source.
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_tile_pixel_y = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint,
                          builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                              builder.makeUintConstant(1)),
                          dest_tile_pixel_y, builder.makeUintConstant(1),
                          builder.makeUintConstant(31));
                    } else {
                      host_depth_source_tile_pixel_y = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint,
                          builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                              builder.makeUintConstant(1)),
                          dest_tile_pixel_y, builder.makeUintConstant(1),
                          builder.makeUintConstant(31));
                    }
                  }
                }
              }
            }
            assert_true(push_constants_member_host_depth_address != UINT32_MAX);
            id_vector_temp.clear();
            id_vector_temp.push_back(
                builder.makeIntConstant(int32_t(push_constants_member_host_depth_address)));
            spv::Id host_depth_address_constant =
                builder.createLoad(builder.createAccessChain(spv::StorageClassPushConstant,
                                                             push_constants, id_vector_temp),
                                   spv::NoPrecision);
            // Transform the destination tile index into the host depth source.
            // After the addition, it may be negative - in which case, the
            // transfer is done across EDRAM addressing wrapping, and
            // xenos::kEdramTileCount must be added to it, but
            // `& (xenos::kEdramTileCount - 1)` handles that regardless of the
            // sign.
            spv::Id host_depth_source_tile_index = builder.createBinOp(
                spv::OpBitwiseAnd, type_uint,
                builder.createUnaryOp(
                    spv::OpBitcast, type_uint,
                    builder.createBinOp(
                        spv::OpIAdd, type_int,
                        builder.createUnaryOp(spv::OpBitcast, type_int, dest_tile_index),
                        builder.createTriOp(
                            spv::OpBitFieldSExtract, type_int,
                            builder.createUnaryOp(spv::OpBitcast, type_int,
                                                  host_depth_address_constant),
                            builder.makeUintConstant(xenos::kEdramPitchTilesBits * 2),
                            builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1)))),
                builder.makeUintConstant(xenos::kEdramTileCount - 1));
            // Split the host depth source tile index into X and Y tile index
            // within the source image.
            spv::Id host_depth_source_pitch_tiles =
                builder.createTriOp(spv::OpBitFieldUExtract, type_uint, host_depth_address_constant,
                                    builder.makeUintConstant(xenos::kEdramPitchTilesBits),
                                    builder.makeUintConstant(xenos::kEdramPitchTilesBits));
            spv::Id host_depth_source_tile_index_y =
                builder.createBinOp(spv::OpUDiv, type_uint, host_depth_source_tile_index,
                                    host_depth_source_pitch_tiles);
            spv::Id host_depth_source_tile_index_x =
                builder.createBinOp(spv::OpUMod, type_uint, host_depth_source_tile_index,
                                    host_depth_source_pitch_tiles);
            // Finally calculate the host depth source texture coordinates.
            spv::Id host_depth_source_pixel_x_int = builder.createUnaryOp(
                spv::OpBitcast, type_int,
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(spv::OpIMul, type_uint,
                                        builder.makeUintConstant(tile_width_samples >>
                                                                 uint32_t(key.source_msaa_samples >=
                                                                          xenos::MsaaSamples::k4X)),
                                        host_depth_source_tile_index_x),
                    host_depth_source_tile_pixel_x));
            spv::Id host_depth_source_pixel_y_int = builder.createUnaryOp(
                spv::OpBitcast, type_int,
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(spv::OpIMul, type_uint,
                                        builder.makeUintConstant(tile_height_samples >>
                                                                 uint32_t(key.source_msaa_samples >=
                                                                          xenos::MsaaSamples::k2X)),
                                        host_depth_source_tile_index_y),
                    host_depth_source_tile_pixel_y));
            // Load the host depth source.
            spv::Builder::TextureParameters host_depth_source_texture_parameters = {};
            host_depth_source_texture_parameters.sampler =
                builder.createLoad(host_depth_source_texture, spv::NoPrecision);
            id_vector_temp.clear();
            id_vector_temp.push_back(host_depth_source_pixel_x_int);
            id_vector_temp.push_back(host_depth_source_pixel_y_int);
            host_depth_source_texture_parameters.coords =
                builder.createCompositeConstruct(type_int2, id_vector_temp);
            if (key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X) {
              host_depth_source_texture_parameters.sample =
                  builder.createUnaryOp(spv::OpBitcast, type_int, host_depth_source_sample_id);
            } else {
              host_depth_source_texture_parameters.lod = builder.makeIntConstant(0);
            }
            host_depth32 = builder.createCompositeExtract(
                builder.createTextureCall(spv::NoPrecision, type_float4, false, true, false, false,
                                          false, host_depth_source_texture_parameters,
                                          spv::ImageOperandsMaskNone),
                type_float, 0);
          } else if (host_depth_source_buffer != spv::NoResult) {
            // Get the address in the EDRAM scratch buffer and load from there.
            // The beginning of the buffer is (0, 0) of the destination.
            // 40-sample columns are not swapped for addressing simplicity
            // (because this is used for depth -> depth transfers, where
            // swapping isn't needed).
            // Convert samples to pixels.
            assert_true(key.host_depth_source_msaa_samples == xenos::MsaaSamples::k1X);
            spv::Id dest_tile_sample_x = dest_tile_pixel_x;
            spv::Id dest_tile_sample_y = dest_tile_pixel_y;
            if (key.dest_msaa_samples >= xenos::MsaaSamples::k2X) {
              if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                // Horizontal sample index in bit 0.
                dest_tile_sample_x = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                    builder.makeUintConstant(1), builder.makeUintConstant(31));
              }
              // Vertical sample index as 1 or 0 in bit 0 for true 2x or as 0
              // or 1 in bit 1 for 4x or for 2x emulated as 4x.
              dest_tile_sample_y = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint,
                  builder.createBinOp((key.dest_msaa_samples == xenos::MsaaSamples::k2X &&
                                       msaa_2x_attachments_supported_)
                                          ? spv::OpBitwiseXor
                                          : spv::OpShiftRightLogical,
                                      type_uint, dest_sample_id, builder.makeUintConstant(1)),
                  dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
            }
            // Combine the tile sample index and the tile index.
            // The tile index doesn't need to be wrapped, as the host depth is
            // written to the beginning of the buffer, without the base offset.
            spv::Id host_depth_offset = builder.createBinOp(
                spv::OpIAdd, type_uint,
                builder.createBinOp(
                    spv::OpIMul, type_uint,
                    builder.makeUintConstant(tile_width_samples * tile_height_samples),
                    dest_tile_index),
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(spv::OpIMul, type_uint,
                                        builder.makeUintConstant(tile_width_samples),
                                        dest_tile_sample_y),
                    dest_tile_sample_x));
            id_vector_temp.clear();
            // The only SSBO structure member.
            id_vector_temp.push_back(builder.makeIntConstant(0));
            id_vector_temp.push_back(
                builder.createUnaryOp(spv::OpBitcast, type_int, host_depth_offset));
            // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is
            // generated, it's Uniform.
            host_depth32 = builder.createUnaryOp(
                spv::OpBitcast, type_float,
                builder.createLoad(
                    builder.createAccessChain(spv::StorageClassUniform, host_depth_source_buffer,
                                              id_vector_temp),
                    spv::NoPrecision));
          }
          spv::Block* depth24_to_depth32_header = builder.getBuildPoint();
          spv::Id depth24_to_depth32_convert_id = spv::NoResult;
          spv::Block* depth24_to_depth32_merge = nullptr;
          spv::Id host_depth24 = spv::NoResult;
          if (host_depth32 != spv::NoResult) {
            // Convert the host depth value to the guest format and check if it
            // matches the value in the currently owning guest render target.
            switch (dest_depth_format) {
              case xenos::DepthRenderTargetFormat::kD24S8: {
                // Round to the nearest even integer. This seems to be the
                // correct conversion, adding +0.5 and rounding towards zero
                // results in red instead of black in the 4D5307E6 clear shader.
                host_depth24 = builder.createUnaryOp(
                    spv::OpConvertFToU, type_uint,
                    builder.createUnaryBuiltinCall(
                        type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                        builder.createBinOp(spv::OpFMul, type_float, host_depth32,
                                            builder.makeFloatConstant(float(0xFFFFFF)))));
              } break;
              case xenos::DepthRenderTargetFormat::kD24FS8: {
                host_depth24 = SpirvShaderTranslator::PreClampedDepthTo20e4(
                    builder, host_depth32, depth_float24_round(), true, ext_inst_glsl_std_450);
              } break;
            }
            assert_true(host_depth24 != spv::NoResult);
            // Update the header block pointer after the conversion (to avoid
            // assuming that the conversion doesn't branch).
            depth24_to_depth32_header = builder.getBuildPoint();
            spv::Id host_depth_outdated =
                builder.createBinOp(spv::OpINotEqual, type_bool, guest_depth24, host_depth24);
            spv::Block& depth24_to_depth32_convert_entry = builder.makeNewBlock();
            {
              spv::Block& depth24_to_depth32_merge_block = builder.makeNewBlock();
              depth24_to_depth32_merge = &depth24_to_depth32_merge_block;
            }
            builder.createSelectionMerge(depth24_to_depth32_merge, spv::SelectionControlMaskNone);
            builder.createConditionalBranch(host_depth_outdated, &depth24_to_depth32_convert_entry,
                                            depth24_to_depth32_merge);
            builder.setBuildPoint(&depth24_to_depth32_convert_entry);
          }
          // Convert the guest 24-bit depth to float32 (in an open conditional
          // if the host depth is also loaded).
          spv::Id guest_depth32 = spv::NoResult;
          switch (dest_depth_format) {
            case xenos::DepthRenderTargetFormat::kD24S8: {
              // Multiplying by 1.0 / 0xFFFFFF produces an incorrect result (for
              // 0xC00000, for instance - which is 2_10_10_10 clear to 0001) -
              // rescale from 0...0xFFFFFF to 0...0x1000000 doing what true
              // float division followed by multiplication does (on x86-64 MSVC
              // with default SSE rounding) - values starting from 0x800000
              // become bigger by 1; then accurately bias the result's exponent.
              guest_depth32 = builder.createBinOp(
                  spv::OpFMul, type_float,
                  builder.createUnaryOp(
                      spv::OpConvertUToF, type_float,
                      builder.createBinOp(
                          spv::OpIAdd, type_uint, guest_depth24,
                          builder.createBinOp(spv::OpShiftRightLogical, type_uint, guest_depth24,
                                              builder.makeUintConstant(23)))),
                  builder.makeFloatConstant(1.0f / float(1 << 24)));
            } break;
            case xenos::DepthRenderTargetFormat::kD24FS8: {
              guest_depth32 = SpirvShaderTranslator::Depth20e4To32(builder, guest_depth24, 0, true,
                                                                   false, ext_inst_glsl_std_450);
            } break;
          }
          assert_true(guest_depth32 != spv::NoResult);
          spv::Id fragment_depth32 = guest_depth32;
          if (host_depth32 != spv::NoResult) {
            assert_not_null(depth24_to_depth32_merge);
            spv::Id depth24_to_depth32_result_block_id = builder.getBuildPoint()->getId();
            builder.createBranch(depth24_to_depth32_merge);
            builder.setBuildPoint(depth24_to_depth32_merge);
            id_vector_temp.clear();
            id_vector_temp.push_back(guest_depth32);
            id_vector_temp.push_back(depth24_to_depth32_result_block_id);
            id_vector_temp.push_back(host_depth32);
            id_vector_temp.push_back(depth24_to_depth32_header->getId());
            fragment_depth32 = builder.createOp(spv::OpPhi, type_float, id_vector_temp);
          }
          builder.createStore(fragment_depth32, output_fragment_depth);
          // Unpack the stencil into the stencil reference output if needed and
          // not already written.
          if (!packed_only_depth && output_fragment_stencil_ref != spv::NoResult) {
            builder.createStore(
                builder.createUnaryOp(spv::OpBitcast, type_int,
                                      builder.createBinOp(spv::OpBitwiseAnd, type_uint, packed,
                                                          builder.makeUintConstant(UINT8_MAX))),
                output_fragment_stencil_ref);
          }
        }
      } break;
      case TransferOutput::kStencilBit: {
        if (packed) {
          // Kill the sample if the needed stencil bit is not set.
          assert_true(push_constants_member_stencil_mask != UINT32_MAX);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.makeIntConstant(int32_t(push_constants_member_stencil_mask)));
          spv::Id stencil_mask_constant =
              builder.createLoad(builder.createAccessChain(spv::StorageClassPushConstant,
                                                           push_constants, id_vector_temp),
                                 spv::NoPrecision);
          SpirvBuilder::IfBuilder stencil_kill_if(
              builder.createBinOp(
                  spv::OpIEqual, type_bool,
                  builder.createBinOp(spv::OpBitwiseAnd, type_uint, packed, stencil_mask_constant),
                  builder.makeUintConstant(0)),
              spv::SelectionControlMaskNone, builder);
          builder.createNoResultOp(spv::OpKill);
          // OpKill terminates the block.
          stencil_kill_if.makeEndIf(false);
        }
      } break;
    }
  }

  // End the main function and make it the entry point.
  builder.leaveFunction();
  builder.addExecutionMode(main_function, spv::ExecutionModeOriginUpperLeft);
  if (output_fragment_depth != spv::NoResult) {
    builder.addExecutionMode(main_function, spv::ExecutionModeDepthReplacing);
  }
  if (output_fragment_stencil_ref != spv::NoResult) {
    builder.addExecutionMode(main_function, spv::ExecutionModeStencilRefReplacingEXT);
  }
  spv::Instruction* entry_point =
      builder.addEntryPoint(spv::ExecutionModelFragment, main_function, "main");
  for (spv::Id interface_id : main_interface) {
    entry_point->addIdOperand(interface_id);
  }

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);

  // Create the shader module, and store the handle even if creation fails not
  // to try to create it again later.
  VkShaderModule shader_module = ui::vulkan::util::CreateShaderModule(
      vulkan_device, reinterpret_cast<const uint32_t*>(shader_code.data()),
      sizeof(uint32_t) * shader_code.size());
  if (shader_module == VK_NULL_HANDLE) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the render target ownership "
        "transfer shader 0x{:08X}",
        key.key);
  }
  transfer_shaders_.emplace(key, shader_module);
  return shader_module;
}

VkPipeline const* VulkanRenderTargetCache::GetTransferPipelines(TransferPipelineKey key) {
  auto pipeline_it = transfer_pipelines_.find(key);
  if (pipeline_it != transfer_pipelines_.end()) {
    return pipeline_it->second[0] != VK_NULL_HANDLE ? pipeline_it->second.data() : nullptr;
  }

  const TransferModeInfo& mode = kTransferModes[size_t(key.shader_key.mode)];

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();
  bool use_dynamic_rendering =
      REXCVAR_GET(vulkan_dynamic_rendering) && device_properties.dynamicRendering;

  VkRenderPass render_pass = VK_NULL_HANDLE;
  if (!use_dynamic_rendering) {
    render_pass = GetHostRenderTargetsRenderPass(key.render_pass_key);
    if (render_pass == VK_NULL_HANDLE) {
      transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
      return nullptr;
    }
  }

  VkShaderModule fragment_shader_module = GetTransferShader(key.shader_key);
  if (fragment_shader_module == VK_NULL_HANDLE) {
    transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
    return nullptr;
  }

  uint32_t dest_sample_count = uint32_t(1) << uint32_t(key.shader_key.dest_msaa_samples);
  bool dest_is_masked_sample = dest_sample_count > 1 && !device_properties.sampleRateShading;

  VkPipelineShaderStageCreateInfo shader_stages[2];
  shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stages[0].pNext = nullptr;
  shader_stages[0].flags = 0;
  shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  shader_stages[0].module = transfer_passthrough_vertex_shader_;
  shader_stages[0].pName = "main";
  shader_stages[0].pSpecializationInfo = nullptr;
  shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stages[1].pNext = nullptr;
  shader_stages[1].flags = 0;
  shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  shader_stages[1].module = fragment_shader_module;
  shader_stages[1].pName = "main";
  shader_stages[1].pSpecializationInfo = nullptr;
  VkSpecializationMapEntry sample_id_specialization_map_entry;
  uint32_t sample_id_specialization_constant;
  VkSpecializationInfo sample_id_specialization_info;
  if (dest_is_masked_sample) {
    sample_id_specialization_map_entry.constantID = 0;
    sample_id_specialization_map_entry.offset = 0;
    sample_id_specialization_map_entry.size = sizeof(uint32_t);
    sample_id_specialization_constant = 0;
    sample_id_specialization_info.mapEntryCount = 1;
    sample_id_specialization_info.pMapEntries = &sample_id_specialization_map_entry;
    sample_id_specialization_info.dataSize = sizeof(sample_id_specialization_constant);
    sample_id_specialization_info.pData = &sample_id_specialization_constant;
    shader_stages[1].pSpecializationInfo = &sample_id_specialization_info;
  }

  VkVertexInputBindingDescription vertex_input_binding;
  vertex_input_binding.binding = 0;
  vertex_input_binding.stride = sizeof(float) * 2;
  vertex_input_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription vertex_input_attribute;
  vertex_input_attribute.location = 0;
  vertex_input_attribute.binding = 0;
  vertex_input_attribute.format = VK_FORMAT_R32G32_SFLOAT;
  vertex_input_attribute.offset = 0;
  VkPipelineVertexInputStateCreateInfo vertex_input_state;
  vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertex_input_state.pNext = nullptr;
  vertex_input_state.flags = 0;
  vertex_input_state.vertexBindingDescriptionCount = 1;
  vertex_input_state.pVertexBindingDescriptions = &vertex_input_binding;
  vertex_input_state.vertexAttributeDescriptionCount = 1;
  vertex_input_state.pVertexAttributeDescriptions = &vertex_input_attribute;

  VkPipelineInputAssemblyStateCreateInfo input_assembly_state;
  input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly_state.pNext = nullptr;
  input_assembly_state.flags = 0;
  input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  input_assembly_state.primitiveRestartEnable = VK_FALSE;

  // Dynamic, to stay within maxViewportDimensions while preferring a
  // power-of-two factor for converting from pixel coordinates to NDC for exact
  // precision.
  VkPipelineViewportStateCreateInfo viewport_state;
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.pNext = nullptr;
  viewport_state.flags = 0;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = nullptr;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo rasterization_state = {};
  rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization_state.cullMode = VK_CULL_MODE_NONE;
  rasterization_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterization_state.lineWidth = 1.0f;

  // For samples other than the first, will be changed for the pipelines for
  // other samples.
  VkSampleMask sample_mask = UINT32_MAX;
  VkPipelineMultisampleStateCreateInfo multisample_state = {};
  multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample_state.rasterizationSamples =
      (dest_sample_count == 2 && !msaa_2x_attachments_supported_)
          ? VK_SAMPLE_COUNT_4_BIT
          : VkSampleCountFlagBits(dest_sample_count);
  if (dest_sample_count > 1) {
    if (device_properties.sampleRateShading) {
      multisample_state.sampleShadingEnable = VK_TRUE;
      multisample_state.minSampleShading = 1.0f;
      if (dest_sample_count == 2 && !msaa_2x_attachments_supported_) {
        // Emulating 2x MSAA as samples 0 and 3 of 4x MSAA when 2x is not
        // supported.
        sample_mask = 0b1001;
      }
    } else {
      sample_mask = 0b1;
    }
    if (sample_mask != UINT32_MAX) {
      multisample_state.pSampleMask = &sample_mask;
    }
  }

  // Whether the depth / stencil state is used depends on the presence of a
  // depth attachment in the render pass - but not making assumptions about
  // whether the render pass contains any specific attachments, so setting up
  // valid depth / stencil state unconditionally.
  VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {};
  depth_stencil_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  if (mode.output == TransferOutput::kDepth) {
    depth_stencil_state.depthTestEnable = VK_TRUE;
    depth_stencil_state.depthWriteEnable = VK_TRUE;
    depth_stencil_state.depthCompareOp =
        REXCVAR_GET(depth_transfer_not_equal_test) ? VK_COMPARE_OP_NOT_EQUAL : VK_COMPARE_OP_ALWAYS;
  }
  if ((mode.output == TransferOutput::kDepth &&
       vulkan_device->extensions().ext_EXT_shader_stencil_export) ||
      mode.output == TransferOutput::kStencilBit) {
    depth_stencil_state.stencilTestEnable = VK_TRUE;
    depth_stencil_state.front.failOp = VK_STENCIL_OP_KEEP;
    depth_stencil_state.front.passOp = VK_STENCIL_OP_REPLACE;
    depth_stencil_state.front.depthFailOp = VK_STENCIL_OP_REPLACE;
    // Using ALWAYS, not NOT_EQUAL, so depth writing is unaffected by stencil
    // being different.
    depth_stencil_state.front.compareOp = VK_COMPARE_OP_ALWAYS;
    // Will be dynamic for stencil bit output.
    depth_stencil_state.front.writeMask = UINT8_MAX;
    depth_stencil_state.front.reference = UINT8_MAX;
    depth_stencil_state.back = depth_stencil_state.front;
  }

  // Whether the color blend state is used depends on the presence of color
  // attachments in the render pass - but not making assumptions about whether
  // the render pass contains any specific attachments, so setting up valid
  // color blend state unconditionally.
  VkPipelineColorBlendAttachmentState color_blend_attachments[xenos::kMaxColorRenderTargets] = {};
  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blend_state.attachmentCount =
      32 - rex::lzcnt(key.render_pass_key.depth_and_color_used >> 1);
  color_blend_state.pAttachments = color_blend_attachments;
  if (mode.output == TransferOutput::kColor) {
    assert_true(device_properties.independentBlend);
    color_blend_attachments[key.shader_key.dest_color_rt_index].colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
  }

  VkPipelineRenderingCreateInfo pipeline_rendering_create_info = {};
  VkFormat color_attachment_format = VK_FORMAT_UNDEFINED;
  VkFormat depth_attachment_format = VK_FORMAT_UNDEFINED;
  VkFormat stencil_attachment_format = VK_FORMAT_UNDEFINED;
  if (use_dynamic_rendering) {
    pipeline_rendering_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pipeline_rendering_create_info.pNext = nullptr;
    pipeline_rendering_create_info.viewMask = 0;
    if (key.render_pass_key.depth_and_color_used & 0b1) {
      depth_attachment_format = GetDepthVulkanFormat(key.render_pass_key.depth_format);
      stencil_attachment_format = depth_attachment_format;
      pipeline_rendering_create_info.colorAttachmentCount = 0;
      pipeline_rendering_create_info.pColorAttachmentFormats = nullptr;
    } else {
      color_attachment_format =
          GetColorOwnershipTransferVulkanFormat(key.render_pass_key.color_0_view_format);
      pipeline_rendering_create_info.colorAttachmentCount = 1;
      pipeline_rendering_create_info.pColorAttachmentFormats = &color_attachment_format;
    }
    pipeline_rendering_create_info.depthAttachmentFormat = depth_attachment_format;
    pipeline_rendering_create_info.stencilAttachmentFormat = stencil_attachment_format;
  }

  std::array<VkDynamicState, 3> dynamic_states;
  VkPipelineDynamicStateCreateInfo dynamic_state;
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.pNext = nullptr;
  dynamic_state.flags = 0;
  dynamic_state.dynamicStateCount = 0;
  dynamic_state.pDynamicStates = dynamic_states.data();
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;
  if (mode.output == TransferOutput::kStencilBit) {
    dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
  }

  std::array<VkPipeline, 4> pipelines{};
  VkGraphicsPipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext = use_dynamic_rendering ? &pipeline_rendering_create_info : nullptr;
  pipeline_create_info.flags = 0;
  if (dest_is_masked_sample) {
    pipeline_create_info.flags |= VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
  }
  pipeline_create_info.stageCount = uint32_t(rex::countof(shader_stages));
  pipeline_create_info.pStages = shader_stages;
  pipeline_create_info.pVertexInputState = &vertex_input_state;
  pipeline_create_info.pInputAssemblyState = &input_assembly_state;
  pipeline_create_info.pTessellationState = nullptr;
  pipeline_create_info.pViewportState = &viewport_state;
  pipeline_create_info.pRasterizationState = &rasterization_state;
  pipeline_create_info.pMultisampleState = &multisample_state;
  pipeline_create_info.pDepthStencilState = &depth_stencil_state;
  pipeline_create_info.pColorBlendState = &color_blend_state;
  pipeline_create_info.pDynamicState = &dynamic_state;
  pipeline_create_info.layout = transfer_pipeline_layouts_[size_t(mode.pipeline_layout)];
  pipeline_create_info.renderPass = use_dynamic_rendering ? VK_NULL_HANDLE : render_pass;
  pipeline_create_info.subpass = 0;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;
  if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr,
                                    &pipelines[0]) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the render target ownership "
        "transfer pipeline for render pass 0x{:08X}, shader 0x{:08X}",
        key.render_pass_key.key, key.shader_key.key);
    transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
    return nullptr;
  }
  if (dest_is_masked_sample) {
    assert_true(multisample_state.pSampleMask == &sample_mask);
    pipeline_create_info.flags =
        (pipeline_create_info.flags & ~VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT) |
        VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    pipeline_create_info.basePipelineHandle = pipelines[0];
    for (uint32_t i = 1; i < dest_sample_count; ++i) {
      // Emulating 2x MSAA as samples 0 and 3 of 4x MSAA when 2x is not
      // supported.
      uint32_t host_sample_index =
          (dest_sample_count == 2 && !msaa_2x_attachments_supported_ && i == 1) ? 3 : i;
      sample_id_specialization_constant = host_sample_index;
      sample_mask = uint32_t(1) << host_sample_index;
      if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr,
                                        &pipelines[i]) != VK_SUCCESS) {
        REXGPU_ERROR(
            "VulkanRenderTargetCache: Failed to create the render target "
            "ownership transfer pipeline for render pass 0x{:08X}, shader "
            "0x{:08X}, sample {}",
            key.render_pass_key.key, key.shader_key.key, i);
        for (uint32_t j = 0; j < i; ++j) {
          dfn.vkDestroyPipeline(device, pipelines[j], nullptr);
        }
        transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
        return nullptr;
      }
    }
  }
  return transfer_pipelines_.emplace(key, pipelines).first->second.data();
}

bool VulkanRenderTargetCache::TryNativeSurfaceCopies(
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers, const Transfer::Rectangle* cutout) {
  if (!native_rt_mode_ || !REXCVAR_GET(native_rt_image_copies) ||
      REXCVAR_GET(native_rt_skip_transfers) || !render_target_transfers ||
      (!REXCVAR_GET(native_rt_size_by_use) && !REXCVAR_GET(native_resolve_copy_free))) {
    return false;
  }
  struct Copy {
    VulkanRenderTarget* source;
    VulkanRenderTarget* dest;
    std::vector<VkImageCopy> regions;
  };
  std::vector<Copy> copies;
  std::vector<NativeSurfaceCopyRegion> surface_regions;
  // Validate the whole batch before recording anything. Mixing an early
  // image copy with a later shader transfer could change cross-copy ordering.
  for (uint32_t i = 0; i < render_target_count; ++i) {
    if (!render_targets[i]) {
      continue;
    }
    auto& dest = *static_cast<VulkanRenderTarget*>(render_targets[i]);
    RenderTargetKey dest_key = dest.key();
    if (render_target_transfers[i].empty()) {
      continue;
    }
    if (!dest_key.is_depth) {
      switch (dest_key.GetColorFormat()) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
          break;
        default:
          return false;
      }
    }
    for (const Transfer& transfer : render_target_transfers[i]) {
      auto& source = *static_cast<VulkanRenderTarget*>(transfer.source);
      RenderTargetKey source_key = source.key();
      if (source.image() == dest.image() || source_key.is_depth != dest_key.is_depth ||
          source_key.resource_format != dest_key.resource_format ||
          source_key.msaa_samples != xenos::MsaaSamples::k1X ||
          dest_key.msaa_samples != xenos::MsaaSamples::k1X ||
          GetRenderTargetScaleX(source_key) != GetRenderTargetScaleX(dest_key) ||
          GetRenderTargetScaleY(source_key) != GetRenderTargetScaleY(dest_key) ||
          (transfer.host_depth_source && transfer.host_depth_source != transfer.source)) {
        return false;
      }
      Copy copy{&source, &dest, {}};
      Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
      uint32_t count =
          transfer.GetRectangles(dest_key.base_tiles, dest_key.GetPitchTiles(),
                                 dest_key.msaa_samples, dest_key.Is64bpp(), rectangles, cutout);
      uint32_t scale_x = GetRenderTargetScaleX(dest_key);
      uint32_t scale_y = GetRenderTargetScaleY(dest_key);
      for (uint32_t j = 0; j < count; ++j) {
        const auto& rectangle = rectangles[j];
        if (!BuildNativeSurfaceCopyRegions(
                source_key.base_tiles, source_key.GetPitchTiles(), source.tile_rows(),
                dest_key.base_tiles, dest_key.GetPitchTiles(), dest.tile_rows(),
                xenos::kEdramTileWidthSamples >> uint32_t(dest_key.Is64bpp()), rectangle.x_pixels,
                rectangle.y_pixels, rectangle.width_pixels, rectangle.height_pixels,
                surface_regions)) {
          return false;
        }
        for (const auto& region : surface_regions) {
          VkImageCopy image_copy = {};
          image_copy.srcSubresource = {
              VkImageAspectFlags(dest_key.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                   : VK_IMAGE_ASPECT_COLOR_BIT),
              0, 0, 1};
          image_copy.dstSubresource = image_copy.srcSubresource;
          image_copy.srcOffset = {int32_t(region.source_x * scale_x),
                                  int32_t(region.source_y * scale_y), 0};
          image_copy.dstOffset = {int32_t(region.dest_x * scale_x),
                                  int32_t(region.dest_y * scale_y), 0};
          image_copy.extent = {region.width * scale_x, region.height * scale_y, 1};
          copy.regions.push_back(image_copy);
          if (dest_key.is_depth) {
            image_copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            image_copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            copy.regions.push_back(image_copy);
          }
        }
      }
      if (!copy.regions.empty()) {
        copies.push_back(std::move(copy));
      }
    }
  }
  if (copies.empty()) {
    return true;
  }
  for (const Copy& copy : copies) {
    VkImageAspectFlags aspects = copy.dest->key().is_depth
                                     ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                     : VK_IMAGE_ASPECT_COLOR_BIT;
    auto transition = [&](VulkanRenderTarget& target, VkImageLayout layout, VkAccessFlags access) {
      command_processor_.PushImageMemoryBarrier(
          target.image(), ui::vulkan::util::InitializeSubresourceRange(aspects),
          target.current_stage_mask(), VK_PIPELINE_STAGE_TRANSFER_BIT, target.current_access_mask(),
          access, target.current_layout(), layout, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
          false);
      target.SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT, access, layout);
    };
    transition(*copy.source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT);
    transition(*copy.dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT);
    command_processor_.SubmitBarriers(true);
    command_processor_.deferred_command_buffer().CmdVkCopyImage(
        copy.source->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy.dest->image(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copy.regions.size()), copy.regions.data());
  }
  // The caller next enters the draw render pass, whose attachments expect
  // their draw layouts. A bound attachment may also have been a copy source.
  for (uint32_t i = 0; i < render_target_count; ++i) {
    if (!render_targets[i]) {
      continue;
    }
    auto& target = *static_cast<VulkanRenderTarget*>(render_targets[i]);
    VkPipelineStageFlags stage;
    VkAccessFlags access;
    VkImageLayout layout;
    target.GetDrawUsage(&stage, &access, &layout);
    command_processor_.PushImageMemoryBarrier(
        target.image(),
        ui::vulkan::util::InitializeSubresourceRange(
            target.key().is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                  : VK_IMAGE_ASPECT_COLOR_BIT),
        target.current_stage_mask(), stage, target.current_access_mask(), access,
        target.current_layout(), layout, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
    target.SetUsage(stage, access, layout);
  }
  static uint64_t batches = 0;
  if (++batches <= 8 || !(batches & 255)) {
    REXGPU_INFO("[native-surface-copy] {} surface copies ({})", copies.size(), batches);
  }
  return true;
}

void VulkanRenderTargetCache::PerformTransfersAndResolveClears(
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers,
    const uint64_t* render_target_resolve_clear_values,
    const Transfer::Rectangle* resolve_clear_rectangle) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  uint64_t current_submission = command_processor_.GetCurrentSubmission();
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();

  bool resolve_clear_needed = render_target_resolve_clear_values && resolve_clear_rectangle;
  // Transfers from or to original resolution render targets would need
  // resampling between the resolution scales (and the host depth store works
  // with the scaled EDRAM buffer layout), so they're dropped. The games they're
  // used for only move dead data that way - a shadow map overlapping the old
  // main depth buffer, or a render target cleared right after taking over.
  std::array<std::vector<Transfer>, 1 + xenos::kMaxColorRenderTargets> scale_kept_transfers;
  if (render_target_transfers) {
    bool any_dropped = false;
    for (uint32_t i = 0; i < render_target_count; ++i) {
      if (!render_targets[i]) {
        continue;
      }
      bool dest_original = render_targets[i]->key().original_resolution;
      for (const Transfer& transfer : render_target_transfers[i]) {
        if (dest_original || transfer.source->key().original_resolution ||
            (transfer.host_depth_source &&
             transfer.host_depth_source->key().original_resolution)) {
          any_dropped = true;
          continue;
        }
        scale_kept_transfers[i].push_back(transfer);
      }
    }
    if (any_dropped) {
      static bool dropped_logged = false;
      if (!dropped_logged) {
        dropped_logged = true;
        REXGPU_INFO(
            "Dropping EDRAM ownership transfers involving original resolution render targets");
      }
      render_target_transfers = scale_kept_transfers.data();
    }
  }
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* render_target = render_targets[i];
    if (!render_target || !render_target->key().is_depth) {
      continue;
    }
    auto& vulkan_render_target = *static_cast<VulkanRenderTarget*>(render_target);
    if (resolve_clear_needed) {
      vulkan_render_target.uniform_stencil().generation = 0;
    } else if (render_target_transfers && !render_target_transfers[i].empty()) {
      ForgetUniformStencilWrittenByTransfers(vulkan_render_target, render_target_transfers[i],
                                             resolve_clear_rectangle);
    }
  }
  // GPU time attribution: everything recorded until the function returns is
  // EDRAM ownership-transfer / resolve-clear work.
  struct TransferProfileScope {
    VulkanCommandProcessor& cp;
    explicit TransferProfileScope(VulkanCommandProcessor& command_processor)
        : cp(command_processor) {
      cp.gpu_profiler().Mark(cp.deferred_command_buffer(),
                             VulkanGpuProfiler::Category::kTransfer);
    }
    ~TransferProfileScope() {
      cp.gpu_profiler().Mark(cp.deferred_command_buffer(), VulkanGpuProfiler::Category::kDraw);
    }
  };
  bool any_transfers = resolve_clear_needed;
  if (render_target_transfers) {
    for (uint32_t i = 0; i < render_target_count && !any_transfers; ++i) {
      any_transfers = !render_target_transfers[i].empty();
    }
  }
  std::optional<TransferProfileScope> transfer_profile_scope;
  if (any_transfers && command_processor_.gpu_profiler().enabled()) {
    transfer_profile_scope.emplace(command_processor_);
  }
  if (!resolve_clear_needed &&
      TryNativeSurfaceCopies(render_target_count, render_targets, render_target_transfers,
                             resolve_clear_rectangle)) {
    return;
  }
  if (render_target_transfers) {
    // Reports EDRAM emulation still at work: each distinct pair of render
    // targets data moves between because the guest reinterprets EDRAM.
    static std::unordered_set<uint64_t> transfer_pairs_logged;
    for (uint32_t i = 0; i < render_target_count; ++i) {
      if (!render_targets[i] || transfer_pairs_logged.size() >= 64) {
        continue;
      }
      RenderTargetKey dest_key = render_targets[i]->key();
      for (const Transfer& transfer : render_target_transfers[i]) {
        RenderTargetKey source_key = transfer.source->key();
        if (transfer_pairs_logged.insert((uint64_t(dest_key.key) << 32) | source_key.key).second) {
          REXGPU_INFO(
              "[native-fallback] EDRAM transfer into the {} render target at {} (pitch {}, {}x "
              "MSAA, format {}) from the {} one at {} (pitch {}, {}x MSAA, format {})",
              dest_key.is_depth ? "depth" : "color", uint32_t(dest_key.base_tiles),
              uint32_t(dest_key.pitch_tiles_at_32bpp), 1u << uint32_t(dest_key.msaa_samples),
              uint32_t(dest_key.resource_format), source_key.is_depth ? "depth" : "color",
              uint32_t(source_key.base_tiles), uint32_t(source_key.pitch_tiles_at_32bpp),
              1u << uint32_t(source_key.msaa_samples), uint32_t(source_key.resource_format));
        }
      }
    }
  }
  if (RtDebugLogActive() && render_target_transfers) {
    for (uint32_t i = 0; i < render_target_count; ++i) {
      if (!render_targets[i]) {
        continue;
      }
      RenderTargetKey dk = render_targets[i]->key();
      for (const Transfer& t : render_target_transfers[i]) {
        RenderTargetKey sk = t.source->key();
        REXGPU_INFO(
            "[rt-debug] transfer dst(base={} pitch={} msaa={} {}) <- src(base={} pitch={} "
            "msaa={} {}) tiles {}..{}{}",
            uint32_t(dk.base_tiles), uint32_t(dk.pitch_tiles_at_32bpp),
            1u << uint32_t(dk.msaa_samples), RtFormatName(dk.is_depth, dk.resource_format),
            uint32_t(sk.base_tiles), uint32_t(sk.pitch_tiles_at_32bpp),
            1u << uint32_t(sk.msaa_samples), RtFormatName(sk.is_depth, sk.resource_format),
            t.start_tiles, t.end_tiles, t.host_depth_source ? " +host_depth" : "");
      }
    }
  }
  if (REXCVAR_GET(native_rt_skip_transfers) && !resolve_clear_needed) {
    // Ownership transfers dropped entirely. The caller has already updated
    // ownership bookkeeping, so skipping just the copy draws keeps state
    // consistent. Counted so the skipped transfers show up in telemetry.
    if (render_target_transfers) {
      for (uint32_t i = 0; i < render_target_count; ++i) {
        transfers_skipped_ += uint32_t(render_target_transfers[i].size());
      }
    }
    return;
  }
  VkClearRect resolve_clear_rect;
  if (resolve_clear_needed) {
    // Assuming the rectangle is already clamped by the setup function from the
    // common render target cache.
    resolve_clear_rect.rect.offset.x =
        int32_t(resolve_clear_rectangle->x_pixels * draw_resolution_scale_x());
    resolve_clear_rect.rect.offset.y =
        int32_t(resolve_clear_rectangle->y_pixels * draw_resolution_scale_y());
    resolve_clear_rect.rect.extent.width =
        resolve_clear_rectangle->width_pixels * draw_resolution_scale_x();
    resolve_clear_rect.rect.extent.height =
        resolve_clear_rectangle->height_pixels * draw_resolution_scale_y();
    resolve_clear_rect.baseArrayLayer = 0;
    resolve_clear_rect.layerCount = 1;
  }

  // Do host depth storing for the depth destination (assuming there can be only
  // one depth destination) where depth destination == host depth source.
  bool host_depth_store_set_up = false;
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    auto& dest_vulkan_rt = *static_cast<VulkanRenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_vulkan_rt.key();
    if (!dest_rt_key.is_depth) {
      continue;
    }
    const std::vector<Transfer>& depth_transfers = render_target_transfers[i];
    for (const Transfer& transfer : depth_transfers) {
      if (transfer.host_depth_source != dest_rt) {
        continue;
      }
      if (!host_depth_store_set_up) {
        // Pipeline.
        command_processor_.BindExternalComputePipeline(
            host_depth_store_pipelines_[size_t(dest_rt_key.msaa_samples)]);
        // Descriptor set bindings.
        VkDescriptorSet host_depth_store_descriptor_sets[] = {
            edram_storage_buffer_descriptor_set_,
            dest_vulkan_rt.GetDescriptorSetTransferSource(),
        };
        command_buffer.CmdVkBindDescriptorSets(
            VK_PIPELINE_BIND_POINT_COMPUTE, host_depth_store_pipeline_layout_, 0,
            uint32_t(rex::countof(host_depth_store_descriptor_sets)),
            host_depth_store_descriptor_sets, 0, nullptr);
        // Render target constant.
        HostDepthStoreRenderTargetConstant host_depth_store_render_target_constant =
            GetHostDepthStoreRenderTargetConstant(dest_rt_key.pitch_tiles_at_32bpp,
                                                  msaa_2x_attachments_supported_);
        command_buffer.CmdVkPushConstants(
            host_depth_store_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
            uint32_t(offsetof(HostDepthStoreConstants, render_target)),
            sizeof(host_depth_store_render_target_constant),
            &host_depth_store_render_target_constant);
        // Barriers - don't need to try to combine them with the rest of
        // render target transfer barriers now - if this happens, after host
        // depth storing, SHADER_READ -> DEPTH_STENCIL_ATTACHMENT_WRITE will be
        // done anyway even in the best case, so it's not possible to have all
        // the barriers in one place here.
        UseEdramBuffer(EdramBufferUsage::kComputeWrite);
        // Always transitioning both depth and stencil, not storing separate
        // usage flags for depth and stencil.
        command_processor_.PushImageMemoryBarrier(
            dest_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                         VK_IMAGE_ASPECT_STENCIL_BIT),
            dest_vulkan_rt.current_stage_mask(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            dest_vulkan_rt.current_access_mask(), VK_ACCESS_SHADER_READ_BIT,
            dest_vulkan_rt.current_layout(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        dest_vulkan_rt.SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        host_depth_store_set_up = true;
      }
      Transfer::Rectangle transfer_rectangles[Transfer::kMaxRectanglesWithCutout];
      uint32_t transfer_rectangle_count = transfer.GetRectangles(
          dest_rt_key.base_tiles, dest_rt_key.pitch_tiles_at_32bpp, dest_rt_key.msaa_samples, false,
          transfer_rectangles, resolve_clear_rectangle);
      assert_not_zero(transfer_rectangle_count);
      HostDepthStoreRectangleConstant host_depth_store_rectangle_constant;
      for (uint32_t j = 0; j < transfer_rectangle_count; ++j) {
        uint32_t group_count_x, group_count_y;
        GetHostDepthStoreRectangleInfo(transfer_rectangles[j], dest_rt_key.msaa_samples,
                                       host_depth_store_rectangle_constant, group_count_x,
                                       group_count_y);
        command_buffer.CmdVkPushConstants(
            host_depth_store_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
            uint32_t(offsetof(HostDepthStoreConstants, rectangle)),
            sizeof(host_depth_store_rectangle_constant), &host_depth_store_rectangle_constant);
        command_processor_.SubmitBarriers(true);
        command_buffer.CmdVkDispatch(group_count_x, group_count_y, 1);
        MarkEdramBufferModified();
      }
    }
    break;
  }

  constexpr VkPipelineStageFlags kSourceStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  constexpr VkAccessFlags kSourceAccessMask = VK_ACCESS_SHADER_READ_BIT;
  constexpr VkImageLayout kSourceLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  // Try to insert as many barriers as possible in one place, hoping that in the
  // best case (no cross-copying between current render targets), barriers will
  // need to be only inserted here, not between transfers. In case of
  // cross-copying, if the destination use is going to happen before the source
  // use, choose the destination state, otherwise the source state - to match
  // the order in which transfers will actually happen (otherwise there will be
  // just a useless switch back and forth).
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    const std::vector<Transfer>& dest_transfers = render_target_transfers[i];
    if (!resolve_clear_needed && dest_transfers.empty()) {
      continue;
    }
    // Transition the destination, only if not going to be used as a source
    // earlier.
    bool dest_used_previously_as_source = false;
    for (uint32_t j = 0; j < i; ++j) {
      for (const Transfer& previous_transfer : render_target_transfers[j]) {
        if (previous_transfer.source == dest_rt || previous_transfer.host_depth_source == dest_rt) {
          dest_used_previously_as_source = true;
          break;
        }
      }
    }
    if (!dest_used_previously_as_source) {
      auto& dest_vulkan_rt = *static_cast<VulkanRenderTarget*>(dest_rt);
      VkPipelineStageFlags dest_dst_stage_mask;
      VkAccessFlags dest_dst_access_mask;
      VkImageLayout dest_new_layout;
      dest_vulkan_rt.GetDrawUsage(&dest_dst_stage_mask, &dest_dst_access_mask, &dest_new_layout);
      command_processor_.PushImageMemoryBarrier(
          dest_vulkan_rt.image(),
          ui::vulkan::util::InitializeSubresourceRange(
              dest_vulkan_rt.key().is_depth
                  ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                  : VK_IMAGE_ASPECT_COLOR_BIT),
          dest_vulkan_rt.current_stage_mask(), dest_dst_stage_mask,
          dest_vulkan_rt.current_access_mask(), dest_dst_access_mask,
          dest_vulkan_rt.current_layout(), dest_new_layout);
      dest_vulkan_rt.SetUsage(dest_dst_stage_mask, dest_dst_access_mask, dest_new_layout);
    }
    // Transition the sources, only if not going to be used as destinations
    // earlier.
    for (const Transfer& transfer : dest_transfers) {
      bool source_previously_used_as_dest = false;
      bool host_depth_source_previously_used_as_dest = false;
      for (uint32_t j = 0; j < i; ++j) {
        if (render_target_transfers[j].empty()) {
          continue;
        }
        const RenderTarget* previous_rt = render_targets[j];
        if (transfer.source == previous_rt) {
          source_previously_used_as_dest = true;
        }
        if (transfer.host_depth_source == previous_rt) {
          host_depth_source_previously_used_as_dest = true;
        }
      }
      if (!source_previously_used_as_dest) {
        auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(transfer.source);
        command_processor_.PushImageMemoryBarrier(
            source_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                source_vulkan_rt.key().is_depth
                    ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                    : VK_IMAGE_ASPECT_COLOR_BIT),
            source_vulkan_rt.current_stage_mask(), kSourceStageMask,
            source_vulkan_rt.current_access_mask(), kSourceAccessMask,
            source_vulkan_rt.current_layout(), kSourceLayout);
        source_vulkan_rt.SetUsage(kSourceStageMask, kSourceAccessMask, kSourceLayout);
      }
      // transfer.host_depth_source == dest_rt means the EDRAM buffer will be
      // used instead, no need to transition.
      if (transfer.host_depth_source && transfer.host_depth_source != dest_rt &&
          !host_depth_source_previously_used_as_dest) {
        auto& host_depth_source_vulkan_rt =
            *static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
        command_processor_.PushImageMemoryBarrier(
            host_depth_source_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                         VK_IMAGE_ASPECT_STENCIL_BIT),
            host_depth_source_vulkan_rt.current_stage_mask(), kSourceStageMask,
            host_depth_source_vulkan_rt.current_access_mask(), kSourceAccessMask,
            host_depth_source_vulkan_rt.current_layout(), kSourceLayout);
        host_depth_source_vulkan_rt.SetUsage(kSourceStageMask, kSourceAccessMask, kSourceLayout);
      }
    }
  }
  if (host_depth_store_set_up) {
    // Will be reading copied host depth from the EDRAM buffer.
    UseEdramBuffer(EdramBufferUsage::kFragmentRead);
  }

  // Perform the transfers and clears.

  TransferPipelineLayoutIndex last_transfer_pipeline_layout_index =
      TransferPipelineLayoutIndex::kCount;
  uint32_t transfer_descriptor_sets_bound = 0;
  uint32_t transfer_push_constants_set = 0;
  VkDescriptorSet last_descriptor_set_host_depth_stencil_textures = VK_NULL_HANDLE;
  VkDescriptorSet last_descriptor_set_depth_stencil_textures = VK_NULL_HANDLE;
  VkDescriptorSet last_descriptor_set_color_texture = VK_NULL_HANDLE;
  TransferAddressConstant last_host_depth_address_constant;
  TransferAddressConstant last_address_constant;

  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }

    const std::vector<Transfer>& current_transfers = render_target_transfers[i];
    if (current_transfers.empty() && !resolve_clear_needed) {
      continue;
    }

    auto& dest_vulkan_rt = *static_cast<VulkanRenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_vulkan_rt.key();

    // Late barriers in case there was cross-copying that prevented merging of
    // barriers.
    {
      VkPipelineStageFlags dest_dst_stage_mask;
      VkAccessFlags dest_dst_access_mask;
      VkImageLayout dest_new_layout;
      dest_vulkan_rt.GetDrawUsage(&dest_dst_stage_mask, &dest_dst_access_mask, &dest_new_layout);
      command_processor_.PushImageMemoryBarrier(
          dest_vulkan_rt.image(),
          ui::vulkan::util::InitializeSubresourceRange(
              dest_rt_key.is_depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                   : VK_IMAGE_ASPECT_COLOR_BIT),
          dest_vulkan_rt.current_stage_mask(), dest_dst_stage_mask,
          dest_vulkan_rt.current_access_mask(), dest_dst_access_mask,
          dest_vulkan_rt.current_layout(), dest_new_layout);
      dest_vulkan_rt.SetUsage(dest_dst_stage_mask, dest_dst_access_mask, dest_new_layout);
    }

    // Get the objects needed for transfers to the destination.
    // TODO(Triang3l): Reuse the guest render pass for transfers where possible
    // (if the Vulkan format used for drawing is also usable for transfers - for
    // instance, R8G8B8A8_UNORM can be used for both, so the guest pass can be
    // reused, but R16G16B16A16_SFLOAT render targets use R16G16B16A16_UINT for
    // transfers, so the transfer pass has to be separate) to avoid stores and
    // loads on tile-based devices to make this actually applicable. Also
    // overall perform all non-cross-copying transfers for the current
    // framebuffer configuration in a single pass, to load / store only once.
    RenderPassKey transfer_render_pass_key;
    transfer_render_pass_key.msaa_samples = dest_rt_key.msaa_samples;
    if (dest_rt_key.is_depth) {
      transfer_render_pass_key.depth_and_color_used = 0b1;
      transfer_render_pass_key.depth_format = dest_rt_key.GetDepthFormat();
    } else {
      transfer_render_pass_key.depth_and_color_used = 0b1 << 1;
      transfer_render_pass_key.color_0_view_format = dest_rt_key.GetColorFormat();
      transfer_render_pass_key.color_rts_use_transfer_formats = 1;
    }
    VkRenderPass transfer_render_pass = GetHostRenderTargetsRenderPass(transfer_render_pass_key);
    if (transfer_render_pass == VK_NULL_HANDLE) {
      continue;
    }
    const RenderTarget* transfer_framebuffer_render_targets[1 + xenos::kMaxColorRenderTargets] = {};
    transfer_framebuffer_render_targets[dest_rt_key.is_depth ? 0 : 1] = dest_rt;
    const Framebuffer* transfer_framebuffer =
        GetHostRenderTargetsFramebuffer(transfer_render_pass_key, dest_rt_key.pitch_tiles_at_32bpp,
                                        transfer_framebuffer_render_targets);
    if (!transfer_framebuffer) {
      continue;
    }
    // Don't enter the render pass immediately - may still insert source
    // barriers later.
    VkImageView transfer_dest_view = dest_rt_key.is_depth ? dest_vulkan_rt.view_depth_stencil()
                                                          : dest_vulkan_rt.view_color_transfer();

    if (!current_transfers.empty()) {
      uint32_t dest_pitch_tiles = dest_rt_key.GetPitchTiles();
      bool dest_is_64bpp = dest_rt_key.Is64bpp();

      // Gather shader keys and sort to reduce pipeline state and binding
      // switches. Also gather stencil rectangles to clear if needed.
      bool need_stencil_bit_draws =
          dest_rt_key.is_depth && !vulkan_device->extensions().ext_EXT_shader_stencil_export;
      current_transfer_invocations_.clear();
      current_transfer_invocations_.reserve(current_transfers.size()
                                            << uint32_t(need_stencil_bit_draws));
      uint32_t rt_sort_index = 0;
      TransferShaderKey new_transfer_shader_key;
      new_transfer_shader_key.dest_msaa_samples = dest_rt_key.msaa_samples;
      new_transfer_shader_key.dest_resource_format = dest_rt_key.resource_format;
      uint32_t stencil_clear_rectangle_count = 0;
      for (uint32_t j = 0; j <= uint32_t(need_stencil_bit_draws); ++j) {
        // j == 0 - color or depth.
        // j == 1 - stencil bits.
        // Stencil bit writing always requires a different root signature,
        // handle these separately. Stencil never has a host depth source.
        // Clear previously set sort indices.
        for (const Transfer& transfer : current_transfers) {
          auto host_depth_source_vulkan_rt =
              static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_vulkan_rt) {
            host_depth_source_vulkan_rt->SetTemporarySortIndex(UINT32_MAX);
          }
          assert_not_null(transfer.source);
          auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(transfer.source);
          source_vulkan_rt.SetTemporarySortIndex(UINT32_MAX);
        }
        for (const Transfer& transfer : current_transfers) {
          assert_not_null(transfer.source);
          auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(transfer.source);
          VulkanRenderTarget* host_depth_source_vulkan_rt =
              j ? nullptr : static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_vulkan_rt &&
              host_depth_source_vulkan_rt->temporary_sort_index() == UINT32_MAX) {
            host_depth_source_vulkan_rt->SetTemporarySortIndex(rt_sort_index++);
          }
          if (source_vulkan_rt.temporary_sort_index() == UINT32_MAX) {
            source_vulkan_rt.SetTemporarySortIndex(rt_sort_index++);
          }
          RenderTargetKey source_rt_key = source_vulkan_rt.key();
          new_transfer_shader_key.source_msaa_samples = source_rt_key.msaa_samples;
          new_transfer_shader_key.source_resource_format = source_rt_key.resource_format;
          bool host_depth_source_is_copy = host_depth_source_vulkan_rt == &dest_vulkan_rt;
          // The host depth copy buffer has only raw samples.
          new_transfer_shader_key.host_depth_source_msaa_samples =
              (host_depth_source_vulkan_rt && !host_depth_source_is_copy)
                  ? host_depth_source_vulkan_rt->key().msaa_samples
                  : xenos::MsaaSamples::k1X;
          if (j) {
            new_transfer_shader_key.mode = source_rt_key.is_depth
                                               ? TransferMode::kDepthToStencilBit
                                               : TransferMode::kColorToStencilBit;
            stencil_clear_rectangle_count += transfer.GetRectangles(
                dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
                nullptr, resolve_clear_rectangle);
          } else {
            if (dest_rt_key.is_depth) {
              if (host_depth_source_vulkan_rt) {
                if (host_depth_source_is_copy) {
                  new_transfer_shader_key.mode = source_rt_key.is_depth
                                                     ? TransferMode::kDepthAndHostDepthCopyToDepth
                                                     : TransferMode::kColorAndHostDepthCopyToDepth;
                } else {
                  new_transfer_shader_key.mode = source_rt_key.is_depth
                                                     ? TransferMode::kDepthAndHostDepthToDepth
                                                     : TransferMode::kColorAndHostDepthToDepth;
                }
              } else {
                new_transfer_shader_key.mode = source_rt_key.is_depth ? TransferMode::kDepthToDepth
                                                                      : TransferMode::kColorToDepth;
              }
            } else {
              new_transfer_shader_key.mode = source_rt_key.is_depth ? TransferMode::kDepthToColor
                                                                    : TransferMode::kColorToColor;
            }
          }
          current_transfer_invocations_.emplace_back(transfer, new_transfer_shader_key);
          if (j) {
            current_transfer_invocations_.back().transfer.host_depth_source = nullptr;
          }
        }
      }
      std::sort(current_transfer_invocations_.begin(), current_transfer_invocations_.end());

      for (auto it = current_transfer_invocations_.cbegin();
           it != current_transfer_invocations_.cend(); ++it) {
        assert_not_null(it->transfer.source);
        auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(it->transfer.source);
        command_processor_.PushImageMemoryBarrier(
            source_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                source_vulkan_rt.key().is_depth
                    ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                    : VK_IMAGE_ASPECT_COLOR_BIT),
            source_vulkan_rt.current_stage_mask(), kSourceStageMask,
            source_vulkan_rt.current_access_mask(), kSourceAccessMask,
            source_vulkan_rt.current_layout(), kSourceLayout);
        source_vulkan_rt.SetUsage(kSourceStageMask, kSourceAccessMask, kSourceLayout);
        auto host_depth_source_vulkan_rt =
            static_cast<VulkanRenderTarget*>(it->transfer.host_depth_source);
        if (host_depth_source_vulkan_rt) {
          TransferShaderKey transfer_shader_key = it->shader_key;
          if (transfer_shader_key.mode == TransferMode::kDepthAndHostDepthCopyToDepth ||
              transfer_shader_key.mode == TransferMode::kColorAndHostDepthCopyToDepth) {
            // Reading copied host depth from the EDRAM buffer.
            UseEdramBuffer(EdramBufferUsage::kFragmentRead);
          } else {
            // Reading host depth from the texture.
            command_processor_.PushImageMemoryBarrier(
                host_depth_source_vulkan_rt->image(),
                ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                             VK_IMAGE_ASPECT_STENCIL_BIT),
                host_depth_source_vulkan_rt->current_stage_mask(), kSourceStageMask,
                host_depth_source_vulkan_rt->current_access_mask(), kSourceAccessMask,
                host_depth_source_vulkan_rt->current_layout(), kSourceLayout);
            host_depth_source_vulkan_rt->SetUsage(kSourceStageMask, kSourceAccessMask,
                                                  kSourceLayout);
          }
        }
      }

      // Perform the transfers for the render target.

      command_processor_.SubmitBarriersAndEnterRenderTargetCacheRenderPass(
          transfer_render_pass, transfer_framebuffer, transfer_dest_view, dest_rt_key.is_depth);

      if (stencil_clear_rectangle_count) {
        VkClearAttachment* stencil_clear_attachment;
        VkClearRect* stencil_clear_rect_write_ptr;
        command_buffer.CmdClearAttachmentsEmplace(1, stencil_clear_attachment,
                                                  stencil_clear_rectangle_count,
                                                  stencil_clear_rect_write_ptr);
        stencil_clear_attachment->aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        stencil_clear_attachment->colorAttachment = 0;
        stencil_clear_attachment->clearValue.depthStencil.depth = 0.0f;
        stencil_clear_attachment->clearValue.depthStencil.stencil = 0;
        for (const Transfer& transfer : current_transfers) {
          Transfer::Rectangle transfer_stencil_clear_rectangles[Transfer::kMaxRectanglesWithCutout];
          uint32_t transfer_stencil_clear_rectangle_count = transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              transfer_stencil_clear_rectangles, resolve_clear_rectangle);
          for (uint32_t j = 0; j < transfer_stencil_clear_rectangle_count; ++j) {
            const Transfer::Rectangle& stencil_clear_rectangle =
                transfer_stencil_clear_rectangles[j];
            stencil_clear_rect_write_ptr->rect.offset.x =
                int32_t(stencil_clear_rectangle.x_pixels * draw_resolution_scale_x());
            stencil_clear_rect_write_ptr->rect.offset.y =
                int32_t(stencil_clear_rectangle.y_pixels * draw_resolution_scale_y());
            stencil_clear_rect_write_ptr->rect.extent.width =
                stencil_clear_rectangle.width_pixels * draw_resolution_scale_x();
            stencil_clear_rect_write_ptr->rect.extent.height =
                stencil_clear_rectangle.height_pixels * draw_resolution_scale_y();
            stencil_clear_rect_write_ptr->baseArrayLayer = 0;
            stencil_clear_rect_write_ptr->layerCount = 1;
            ++stencil_clear_rect_write_ptr;
          }
        }
      }

      // Prefer power of two viewports for exact division by simply biasing the
      // exponent.
      VkViewport transfer_viewport;
      transfer_viewport.x = 0.0f;
      transfer_viewport.y = 0.0f;
      transfer_viewport.width =
          float(std::min(rex::next_pow2(transfer_framebuffer->host_extent.width),
                         vulkan_device->properties().maxViewportDimensions[0]));
      transfer_viewport.height =
          float(std::min(rex::next_pow2(transfer_framebuffer->host_extent.height),
                         vulkan_device->properties().maxViewportDimensions[1]));
      transfer_viewport.minDepth = 0.0f;
      transfer_viewport.maxDepth = 1.0f;
      command_processor_.SetViewport(transfer_viewport);
      float pixels_to_ndc_x = (2.0f / transfer_viewport.width) * float(draw_resolution_scale_x());
      float pixels_to_ndc_y = (2.0f / transfer_viewport.height) * float(draw_resolution_scale_y());
      VkRect2D transfer_scissor;
      transfer_scissor.offset.x = 0;
      transfer_scissor.offset.y = 0;
      transfer_scissor.extent = transfer_framebuffer->host_extent;
      command_processor_.SetScissor(transfer_scissor);

      for (auto it = current_transfer_invocations_.cbegin();
           it != current_transfer_invocations_.cend(); ++it) {
        const TransferInvocation& transfer_invocation_first = *it;
        // Will be merging transfers from the same source into one mesh.
        auto it_merged_first = it, it_merged_last = it;
        uint32_t transfer_rectangle_count = transfer_invocation_first.transfer.GetRectangles(
            dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
            nullptr, resolve_clear_rectangle);
        for (auto it_merge = std::next(it_merged_first);
             it_merge != current_transfer_invocations_.cend(); ++it_merge) {
          if (!transfer_invocation_first.CanBeMergedIntoOneDraw(*it_merge)) {
            break;
          }
          transfer_rectangle_count += it_merge->transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              nullptr, resolve_clear_rectangle);
          it_merged_last = it_merge;
        }
        assert_not_zero(transfer_rectangle_count);
        // Skip the merged transfers in the subsequent iterations.
        it = it_merged_last;

        assert_not_null(it->transfer.source);
        auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(it->transfer.source);
        auto host_depth_source_vulkan_rt =
            static_cast<VulkanRenderTarget*>(it->transfer.host_depth_source);
        TransferShaderKey transfer_shader_key = it->shader_key;
        const TransferModeInfo& transfer_mode_info =
            kTransferModes[size_t(transfer_shader_key.mode)];
        TransferPipelineLayoutIndex transfer_pipeline_layout_index =
            transfer_mode_info.pipeline_layout;
        const TransferPipelineLayoutInfo& transfer_pipeline_layout_info =
            kTransferPipelineLayoutInfos[size_t(transfer_pipeline_layout_index)];
        uint32_t transfer_sample_pipeline_count = vulkan_device->properties().sampleRateShading
                                                      ? 1
                                                      : uint32_t(1)
                                                            << uint32_t(dest_rt_key.msaa_samples);
        bool transfer_is_stencil_bit = (transfer_pipeline_layout_info.used_push_constant_dwords &
                                        kTransferUsedPushConstantDwordStencilMaskBit) != 0;

        uint32_t transfer_vertex_count = 6 * transfer_rectangle_count;
        VkBuffer transfer_vertex_buffer;
        VkDeviceSize transfer_vertex_buffer_offset;
        float* transfer_rectangle_write_ptr =
            reinterpret_cast<float*>(transfer_vertex_buffer_pool_->Request(
                current_submission, sizeof(float) * 2 * transfer_vertex_count, sizeof(float),
                transfer_vertex_buffer, transfer_vertex_buffer_offset));
        if (!transfer_rectangle_write_ptr) {
          continue;
        }
        for (auto it_merged = it_merged_first; it_merged <= it_merged_last; ++it_merged) {
          Transfer::Rectangle transfer_invocation_rectangles[Transfer::kMaxRectanglesWithCutout];
          uint32_t transfer_invocation_rectangle_count = it_merged->transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              transfer_invocation_rectangles, resolve_clear_rectangle);
          assert_not_zero(transfer_invocation_rectangle_count);
          for (uint32_t j = 0; j < transfer_invocation_rectangle_count; ++j) {
            const Transfer::Rectangle& transfer_rectangle = transfer_invocation_rectangles[j];
            float transfer_rectangle_x0 = -1.0f + transfer_rectangle.x_pixels * pixels_to_ndc_x;
            float transfer_rectangle_y0 = -1.0f + transfer_rectangle.y_pixels * pixels_to_ndc_y;
            float transfer_rectangle_x1 =
                transfer_rectangle_x0 + transfer_rectangle.width_pixels * pixels_to_ndc_x;
            float transfer_rectangle_y1 =
                transfer_rectangle_y0 + transfer_rectangle.height_pixels * pixels_to_ndc_y;
            // O-*
            // |/
            // *
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            // *-*
            // |/
            // O
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            // *-O
            // |/
            // *
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            //   O
            //  /|
            // *-*
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            //   *
            //  /|
            // O-*
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            //   *
            //  /|
            // *-O
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
          }
        }
        command_buffer.CmdVkBindVertexBuffers(0, 1, &transfer_vertex_buffer,
                                              &transfer_vertex_buffer_offset);

        const VkPipeline* transfer_pipelines = GetTransferPipelines(
            TransferPipelineKey(transfer_render_pass_key, transfer_shader_key));
        if (!transfer_pipelines) {
          continue;
        }
        command_processor_.BindExternalGraphicsPipeline(transfer_pipelines[0]);
        if (last_transfer_pipeline_layout_index != transfer_pipeline_layout_index) {
          last_transfer_pipeline_layout_index = transfer_pipeline_layout_index;
          transfer_descriptor_sets_bound = 0;
          transfer_push_constants_set = 0;
        }

        // Invalidate outdated bindings.
        if (transfer_pipeline_layout_info.used_descriptor_sets &
            kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
          assert_not_null(host_depth_source_vulkan_rt);
          VkDescriptorSet descriptor_set_host_depth_stencil_textures =
              host_depth_source_vulkan_rt->GetDescriptorSetTransferSource();
          if (last_descriptor_set_host_depth_stencil_textures !=
              descriptor_set_host_depth_stencil_textures) {
            last_descriptor_set_host_depth_stencil_textures =
                descriptor_set_host_depth_stencil_textures;
            transfer_descriptor_sets_bound &=
                ~kTransferUsedDescriptorSetHostDepthStencilTexturesBit;
          }
        }
        if (transfer_pipeline_layout_info.used_descriptor_sets &
            kTransferUsedDescriptorSetDepthStencilTexturesBit) {
          VkDescriptorSet descriptor_set_depth_stencil_textures =
              source_vulkan_rt.GetDescriptorSetTransferSource();
          if (last_descriptor_set_depth_stencil_textures != descriptor_set_depth_stencil_textures) {
            last_descriptor_set_depth_stencil_textures = descriptor_set_depth_stencil_textures;
            transfer_descriptor_sets_bound &= ~kTransferUsedDescriptorSetDepthStencilTexturesBit;
          }
        }
        if (transfer_pipeline_layout_info.used_descriptor_sets &
            kTransferUsedDescriptorSetColorTextureBit) {
          VkDescriptorSet descriptor_set_color_texture =
              source_vulkan_rt.GetDescriptorSetTransferSource();
          if (last_descriptor_set_color_texture != descriptor_set_color_texture) {
            last_descriptor_set_color_texture = descriptor_set_color_texture;
            transfer_descriptor_sets_bound &= ~kTransferUsedDescriptorSetColorTextureBit;
          }
        }
        if (transfer_pipeline_layout_info.used_push_constant_dwords &
            kTransferUsedPushConstantDwordHostDepthAddressBit) {
          assert_not_null(host_depth_source_vulkan_rt);
          RenderTargetKey host_depth_source_rt_key = host_depth_source_vulkan_rt->key();
          TransferAddressConstant host_depth_address_constant;
          host_depth_address_constant.dest_pitch = dest_pitch_tiles;
          host_depth_address_constant.source_pitch = host_depth_source_rt_key.GetPitchTiles();
          host_depth_address_constant.source_to_dest =
              int32_t(dest_rt_key.base_tiles) - int32_t(host_depth_source_rt_key.base_tiles);
          if (last_host_depth_address_constant != host_depth_address_constant) {
            last_host_depth_address_constant = host_depth_address_constant;
            transfer_push_constants_set &= ~kTransferUsedPushConstantDwordHostDepthAddressBit;
          }
        }
        if (transfer_pipeline_layout_info.used_push_constant_dwords &
            kTransferUsedPushConstantDwordAddressBit) {
          RenderTargetKey source_rt_key = source_vulkan_rt.key();
          TransferAddressConstant address_constant;
          address_constant.dest_pitch = dest_pitch_tiles;
          address_constant.source_pitch = source_rt_key.GetPitchTiles();
          address_constant.source_to_dest =
              int32_t(dest_rt_key.base_tiles) - int32_t(source_rt_key.base_tiles);
          if (last_address_constant != address_constant) {
            last_address_constant = address_constant;
            transfer_push_constants_set &= ~kTransferUsedPushConstantDwordAddressBit;
          }
        }

        // Apply the new bindings.
        // TODO(Triang3l): Merge binding updates into spans.
        VkPipelineLayout transfer_pipeline_layout =
            transfer_pipeline_layouts_[size_t(transfer_pipeline_layout_index)];
        uint32_t transfer_descriptor_sets_unbound =
            transfer_pipeline_layout_info.used_descriptor_sets & ~transfer_descriptor_sets_bound;
        if (transfer_descriptor_sets_unbound & kTransferUsedDescriptorSetHostDepthBufferBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              rex::bit_count(transfer_pipeline_layout_info.used_descriptor_sets &
                             (kTransferUsedDescriptorSetHostDepthBufferBit - 1)),
              1, &edram_storage_buffer_descriptor_set_, 0, nullptr);
          transfer_descriptor_sets_bound |= kTransferUsedDescriptorSetHostDepthBufferBit;
        }
        if (transfer_descriptor_sets_unbound &
            kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              rex::bit_count(transfer_pipeline_layout_info.used_descriptor_sets &
                             (kTransferUsedDescriptorSetHostDepthStencilTexturesBit - 1)),
              1, &last_descriptor_set_host_depth_stencil_textures, 0, nullptr);
          transfer_descriptor_sets_bound |= kTransferUsedDescriptorSetHostDepthStencilTexturesBit;
        }
        if (transfer_descriptor_sets_unbound & kTransferUsedDescriptorSetDepthStencilTexturesBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              rex::bit_count(transfer_pipeline_layout_info.used_descriptor_sets &
                             (kTransferUsedDescriptorSetDepthStencilTexturesBit - 1)),
              1, &last_descriptor_set_depth_stencil_textures, 0, nullptr);
          transfer_descriptor_sets_bound |= kTransferUsedDescriptorSetDepthStencilTexturesBit;
        }
        if (transfer_descriptor_sets_unbound & kTransferUsedDescriptorSetColorTextureBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              rex::bit_count(transfer_pipeline_layout_info.used_descriptor_sets &
                             (kTransferUsedDescriptorSetColorTextureBit - 1)),
              1, &last_descriptor_set_color_texture, 0, nullptr);
          transfer_descriptor_sets_bound |= kTransferUsedDescriptorSetColorTextureBit;
        }
        uint32_t transfer_push_constants_unset =
            transfer_pipeline_layout_info.used_push_constant_dwords & ~transfer_push_constants_set;
        if (transfer_push_constants_unset & kTransferUsedPushConstantDwordHostDepthAddressBit) {
          command_buffer.CmdVkPushConstants(
              transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
              sizeof(uint32_t) *
                  rex::bit_count(transfer_pipeline_layout_info.used_push_constant_dwords &
                                 (kTransferUsedPushConstantDwordHostDepthAddressBit - 1)),
              sizeof(uint32_t), &last_host_depth_address_constant);
          transfer_push_constants_set |= kTransferUsedPushConstantDwordHostDepthAddressBit;
        }
        if (transfer_push_constants_unset & kTransferUsedPushConstantDwordAddressBit) {
          command_buffer.CmdVkPushConstants(
              transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
              sizeof(uint32_t) *
                  rex::bit_count(transfer_pipeline_layout_info.used_push_constant_dwords &
                                 (kTransferUsedPushConstantDwordAddressBit - 1)),
              sizeof(uint32_t), &last_address_constant);
          transfer_push_constants_set |= kTransferUsedPushConstantDwordAddressBit;
        }

        for (uint32_t j = 0; j < transfer_sample_pipeline_count; ++j) {
          if (j) {
            command_processor_.BindExternalGraphicsPipeline(transfer_pipelines[j]);
          }
          for (uint32_t k = 0; k < uint32_t(transfer_is_stencil_bit ? 8 : 1); ++k) {
            if (transfer_is_stencil_bit) {
              uint32_t transfer_stencil_bit = uint32_t(1) << k;
              command_buffer.CmdVkPushConstants(
                  transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                  sizeof(uint32_t) *
                      rex::bit_count(transfer_pipeline_layout_info.used_push_constant_dwords &
                                     (kTransferUsedPushConstantDwordStencilMaskBit - 1)),
                  sizeof(uint32_t), &transfer_stencil_bit);
              command_buffer.CmdVkSetStencilWriteMask(VK_STENCIL_FACE_FRONT_AND_BACK,
                                                      transfer_stencil_bit);
            }
            g_edram_transfer_draws.fetch_add(1, std::memory_order_relaxed);
            // NATIVE RT PATH GROUNDWORK: characterise each transfer so the
            // native path can decide which are genuinely required (real EDRAM
            // aliasing) and which are redundant re-copies of a range nothing
            // else touched. Logged sparsely; the pattern repeats every frame.
            {
              static std::atomic<uint32_t> xfer_log{0};
              uint32_t n = xfer_log.fetch_add(1, std::memory_order_relaxed);
              if (n < 40) {
                REXGPU_INFO("[edram-xfer] #{} dest_rt=0x{:08X} verts={} rects={}",
                            n, dest_rt_key.key, transfer_vertex_count,
                            transfer_vertex_count / 3);
              }
            }
            command_buffer.CmdVkDraw(transfer_vertex_count, 1, 0, 0);
          }
        }
      }
    }

    // Perform the clear.
    if (resolve_clear_needed) {
      if (dest_rt_key.original_resolution) {
        resolve_clear_rect.rect.offset.x = int32_t(resolve_clear_rectangle->x_pixels);
        resolve_clear_rect.rect.offset.y = int32_t(resolve_clear_rectangle->y_pixels);
        resolve_clear_rect.rect.extent.width = resolve_clear_rectangle->width_pixels;
        resolve_clear_rect.rect.extent.height = resolve_clear_rectangle->height_pixels;
      } else {
        resolve_clear_rect.rect.offset.x =
            int32_t(resolve_clear_rectangle->x_pixels * draw_resolution_scale_x());
        resolve_clear_rect.rect.offset.y =
            int32_t(resolve_clear_rectangle->y_pixels * draw_resolution_scale_y());
        resolve_clear_rect.rect.extent.width =
            resolve_clear_rectangle->width_pixels * draw_resolution_scale_x();
        resolve_clear_rect.rect.extent.height =
            resolve_clear_rectangle->height_pixels * draw_resolution_scale_y();
      }
      command_processor_.SubmitBarriersAndEnterRenderTargetCacheRenderPass(
          transfer_render_pass, transfer_framebuffer, transfer_dest_view, dest_rt_key.is_depth);
      VkClearAttachment resolve_clear_attachment;
      resolve_clear_attachment.colorAttachment = 0;
      std::memset(&resolve_clear_attachment.clearValue, 0,
                  sizeof(resolve_clear_attachment.clearValue));
      uint64_t clear_value = render_target_resolve_clear_values[i];
      if (dest_rt_key.is_depth) {
        resolve_clear_attachment.aspectMask =
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        uint32_t depth_guest_clear_value = (uint32_t(clear_value) >> 8) & 0xFFFFFF;
        switch (dest_rt_key.GetDepthFormat()) {
          case xenos::DepthRenderTargetFormat::kD24S8:
            resolve_clear_attachment.clearValue.depthStencil.depth =
                xenos::UNorm24To32(depth_guest_clear_value);
            break;
          case xenos::DepthRenderTargetFormat::kD24FS8:
            // Taking [0, 2) -> [0, 1) remapping into account.
            resolve_clear_attachment.clearValue.depthStencil.depth =
                xenos::Float20e4To32(depth_guest_clear_value) * 0.5f;
            break;
        }
        resolve_clear_attachment.clearValue.depthStencil.stencil = uint32_t(clear_value) & 0xFF;
      } else {
        resolve_clear_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bool dest_color_transfer_is_uint = false;
        GetColorOwnershipTransferVulkanFormat(
            dest_rt_key.GetColorFormat(), dest_rt_key.msaa_samples, &dest_color_transfer_is_uint);
        switch (dest_rt_key.GetColorFormat()) {
          case xenos::ColorRenderTargetFormat::k_8_8_8_8:
          case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
            for (uint32_t j = 0; j < 4; ++j) {
              resolve_clear_attachment.clearValue.color.float32[j] =
                  ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
            if (dest_rt_key.GetColorFormat() == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                gamma_render_target_as_unorm16_) {
              // 8_8_8_8_GAMMA is represented by linear stored in
              // R16G16B16A16_UNORM.
              for (uint32_t j = 0; j < 3; ++j) {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    xenos::PWLGammaToLinear(resolve_clear_attachment.clearValue.color.float32[j]);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
            for (uint32_t j = 0; j < 3; ++j) {
              resolve_clear_attachment.clearValue.color.float32[j] =
                  ((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
            }
            resolve_clear_attachment.clearValue.color.float32[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
            for (uint32_t j = 0; j < 3; ++j) {
              resolve_clear_attachment.clearValue.color.float32[j] =
                  xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
            }
            resolve_clear_attachment.clearValue.color.float32[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
            // Using uint for transfers and clears of both. Disregarding the
            // current -32...32 vs. -1...1 settings for consistency with color
            // clear via depth aliasing.
            for (uint32_t j = 0; j < 2; ++j) {
              uint16_t component = uint16_t((clear_value >> (j * 16)) & 0xFFFF);
              if (dest_color_transfer_is_uint) {
                resolve_clear_attachment.clearValue.color.uint32[j] = component;
              } else if (IsColor16FormatFloatLike(dest_rt_key.GetColorFormat())) {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    rex::xenos_half_to_float(component);
              } else {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    std::max(float(int16_t(component)) * (1.0f / 32767.0f), -1.0f);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
            // Using uint for transfers and clears of both. Disregarding the
            // current -32...32 vs. -1...1 settings for consistency with color
            // clear via depth aliasing.
            for (uint32_t j = 0; j < 4; ++j) {
              uint16_t component = uint16_t((clear_value >> (j * 16)) & 0xFFFF);
              if (dest_color_transfer_is_uint) {
                resolve_clear_attachment.clearValue.color.uint32[j] = component;
              } else if (IsColor16FormatFloatLike(dest_rt_key.GetColorFormat())) {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    rex::xenos_half_to_float(component);
              } else {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    std::max(float(int16_t(component)) * (1.0f / 32767.0f), -1.0f);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
            uint32_t component = uint32_t(clear_value);
            if (dest_color_transfer_is_uint) {
              // Using uint for proper denormal and NaN handling.
              resolve_clear_attachment.clearValue.color.uint32[0] = component;
            } else {
              std::memcpy(&resolve_clear_attachment.clearValue.color.float32[0], &component,
                          sizeof(component));
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
            uint32_t component_0 = uint32_t(clear_value);
            uint32_t component_1 = uint32_t(clear_value >> 32);
            if (dest_color_transfer_is_uint) {
              // Using uint for proper denormal and NaN handling.
              resolve_clear_attachment.clearValue.color.uint32[0] = component_0;
              resolve_clear_attachment.clearValue.color.uint32[1] = component_1;
            } else {
              std::memcpy(&resolve_clear_attachment.clearValue.color.float32[0], &component_0,
                          sizeof(component_0));
              std::memcpy(&resolve_clear_attachment.clearValue.color.float32[1], &component_1,
                          sizeof(component_1));
            }
          } break;
        }
      }
      command_buffer.CmdVkClearAttachments(1, &resolve_clear_attachment, 1, &resolve_clear_rect);
    }
  }
}

VkPipeline VulkanRenderTargetCache::GetDumpPipeline(DumpPipelineKey key) {
  auto pipeline_it = dump_pipelines_.find(key);
  if (pipeline_it != dump_pipelines_.end()) {
    return pipeline_it->second;
  }

  std::vector<spv::Id> id_vector_temp;

  SpirvBuilder builder(spv::Spv_1_0, (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1, nullptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityShader);
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_int2 = builder.makeVectorType(type_int, 2);
  spv::Id type_uint = builder.makeUintType(32);
  spv::Id type_uint2 = builder.makeVectorType(type_uint, 2);
  spv::Id type_uint3 = builder.makeVectorType(type_uint, 3);
  spv::Id type_float = builder.makeFloatType(32);

  // Bindings.
  // EDRAM buffer.
  bool format_is_64bpp =
      !key.is_depth && xenos::IsColorRenderTargetFormat64bpp(key.GetColorFormat());
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeRuntimeArray(format_is_64bpp ? type_uint2 : type_uint));
  // Storage buffers have std430 packing, no padding to 4-component vectors.
  builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride,
                        sizeof(uint32_t) << uint32_t(format_is_64bpp));
  spv::Id type_edram = builder.makeStructType(id_vector_temp, "XeEdram");
  builder.addMemberName(type_edram, 0, "edram");
  builder.addMemberDecoration(type_edram, 0, spv::DecorationNonReadable);
  builder.addMemberDecoration(type_edram, 0, spv::DecorationOffset, 0);
  // Block since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // BufferBlock.
  builder.addDecoration(type_edram, spv::DecorationBufferBlock);
  // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // Uniform.
  spv::Id edram_buffer =
      builder.createVariable(spv::NoPrecision, spv::StorageClassUniform, type_edram, "xe_edram");
  builder.addDecoration(edram_buffer, spv::DecorationDescriptorSet, kDumpDescriptorSetEdram);
  builder.addDecoration(edram_buffer, spv::DecorationBinding, 0);
  // Color or depth source.
  bool source_is_multisampled = key.msaa_samples != xenos::MsaaSamples::k1X;
  bool source_is_uint;
  if (key.is_depth) {
    source_is_uint = false;
  } else {
    GetColorOwnershipTransferVulkanFormat(key.GetColorFormat(), key.msaa_samples, &source_is_uint);
  }
  spv::Id source_component_type = source_is_uint ? type_uint : type_float;
  spv::Id source_texture = builder.createVariable(
      spv::NoPrecision, spv::StorageClassUniformConstant,
      builder.makeImageType(source_component_type, spv::Dim2D, false, false, source_is_multisampled,
                            1, spv::ImageFormatUnknown),
      "xe_edram_dump_source");
  builder.addDecoration(source_texture, spv::DecorationDescriptorSet, kDumpDescriptorSetSource);
  builder.addDecoration(source_texture, spv::DecorationBinding, 0);
  // Stencil source.
  spv::Id source_stencil_texture = spv::NoResult;
  if (key.is_depth) {
    source_stencil_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(type_uint, spv::Dim2D, false, false, source_is_multisampled, 1,
                              spv::ImageFormatUnknown),
        "xe_edram_dump_stencil");
    builder.addDecoration(source_stencil_texture, spv::DecorationDescriptorSet,
                          kDumpDescriptorSetSource);
    builder.addDecoration(source_stencil_texture, spv::DecorationBinding, 1);
  }
  // Push constants.
  id_vector_temp.clear();
  id_vector_temp.reserve(kDumpPushConstantCount);
  for (uint32_t i = 0; i < kDumpPushConstantCount; ++i) {
    id_vector_temp.push_back(type_uint);
  }
  spv::Id type_push_constants = builder.makeStructType(id_vector_temp, "XeEdramDumpPushConstants");
  builder.addMemberName(type_push_constants, kDumpPushConstantPitches, "pitches");
  builder.addMemberDecoration(type_push_constants, kDumpPushConstantPitches, spv::DecorationOffset,
                              int(sizeof(uint32_t) * kDumpPushConstantPitches));
  builder.addMemberName(type_push_constants, kDumpPushConstantOffsets, "offsets");
  builder.addMemberDecoration(type_push_constants, kDumpPushConstantOffsets, spv::DecorationOffset,
                              int(sizeof(uint32_t) * kDumpPushConstantOffsets));
  builder.addDecoration(type_push_constants, spv::DecorationBlock);
  spv::Id push_constants =
      builder.createVariable(spv::NoPrecision, spv::StorageClassPushConstant, type_push_constants,
                             "xe_edram_dump_push_constants");

  // gl_GlobalInvocationID input.
  spv::Id input_global_invocation_id = builder.createVariable(
      spv::NoPrecision, spv::StorageClassInput, type_uint3, "gl_GlobalInvocationID");
  builder.addDecoration(input_global_invocation_id, spv::DecorationBuiltIn,
                        spv::BuiltInGlobalInvocationId);

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function = builder.makeFunctionEntry(
      spv::NoPrecision, type_void, "main", main_param_types, main_precisions, &main_entry);

  // For now, as the exact addressing in 64bpp render targets relatively to
  // 32bpp is unknown, treating 64bpp tiles as storing 40x16 samples rather than
  // 80x16 for simplicity of addressing into the texture.

  // Split the destination sample index into the 32bpp tile and the
  // 32bpp-tile-relative sample index.
  // Note that division by non-power-of-two constants will include a 4-cycle
  // 32*32 multiplication on AMD, even though so many bits are not needed for
  // the sample position - however, if an OpUnreachable path is inserted for the
  // case when the position has upper bits set, for some reason, the code for it
  // is not eliminated when compiling the shader for AMD via RenderDoc on
  // Windows, as of June 2022.
  spv::Id global_invocation_id = builder.createLoad(input_global_invocation_id, spv::NoPrecision);
  spv::Id rectangle_sample_x = builder.createCompositeExtract(global_invocation_id, type_uint, 0);
  uint32_t tile_width =
      (xenos::kEdramTileWidthSamples >> uint32_t(format_is_64bpp)) * draw_resolution_scale_x();
  spv::Id const_tile_width = builder.makeUintConstant(tile_width);
  spv::Id rectangle_tile_index_x =
      builder.createBinOp(spv::OpUDiv, type_uint, rectangle_sample_x, const_tile_width);
  spv::Id tile_sample_x =
      builder.createBinOp(spv::OpUMod, type_uint, rectangle_sample_x, const_tile_width);
  spv::Id rectangle_sample_y = builder.createCompositeExtract(global_invocation_id, type_uint, 1);
  uint32_t tile_height = xenos::kEdramTileHeightSamples * draw_resolution_scale_y();
  spv::Id const_tile_height = builder.makeUintConstant(tile_height);
  spv::Id rectangle_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, rectangle_sample_y, const_tile_height);
  spv::Id tile_sample_y =
      builder.createBinOp(spv::OpUMod, type_uint, rectangle_sample_y, const_tile_height);

  // Get the tile index in the EDRAM relative to the dump rectangle base tile.
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeIntConstant(kDumpPushConstantPitches));
  spv::Id pitches_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants, id_vector_temp),
      spv::NoPrecision);
  spv::Id const_uint_0 = builder.makeUintConstant(0);
  spv::Id const_edram_pitch_tiles_bits = builder.makeUintConstant(xenos::kEdramPitchTilesBits);
  spv::Id rectangle_tile_index = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint,
                          builder.createTriOp(spv::OpBitFieldUExtract, type_uint, pitches_constant,
                                              const_uint_0, const_edram_pitch_tiles_bits),
                          rectangle_tile_index_y),
      rectangle_tile_index_x);
  // Add the base tile in the dispatch to the dispatch-local tile index, not
  // wrapping yet so in case of a wraparound, the address relative to the base
  // in the image after subtraction of the base won't be negative.
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeIntConstant(kDumpPushConstantOffsets));
  spv::Id offsets_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants, id_vector_temp),
      spv::NoPrecision);
  spv::Id const_edram_base_tiles_bits_plus_1 =
      builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1);
  spv::Id edram_tile_index_non_wrapped =
      builder.createBinOp(spv::OpIAdd, type_uint,
                          builder.createTriOp(spv::OpBitFieldUExtract, type_uint, offsets_constant,
                                              const_uint_0, const_edram_base_tiles_bits_plus_1),
                          rectangle_tile_index);

  // Combine the tile sample index and the tile index, wrapping the tile
  // addressing, into the EDRAM sample index.
  spv::Id edram_sample_address = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint, builder.makeUintConstant(tile_width * tile_height),
          builder.createBinOp(spv::OpBitwiseAnd, type_uint, edram_tile_index_non_wrapped,
                              builder.makeUintConstant(xenos::kEdramTileCount - 1))),
      builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(spv::OpIMul, type_uint, const_tile_width, tile_sample_y),
          tile_sample_x));
  if (key.is_depth) {
    // Swap 40-sample columns in the depth buffer in the destination address to
    // get the final address of the sample in the EDRAM.
    uint32_t tile_width_half = tile_width >> 1;
    edram_sample_address = builder.createUnaryOp(
        spv::OpBitcast, type_uint,
        builder.createBinOp(
            spv::OpIAdd, type_int,
            builder.createUnaryOp(spv::OpBitcast, type_int, edram_sample_address),
            builder.createTriOp(
                spv::OpSelect, type_int,
                builder.createBinOp(spv::OpULessThan, builder.makeBoolType(), tile_sample_x,
                                    builder.makeUintConstant(tile_width_half)),
                builder.makeIntConstant(int32_t(tile_width_half)),
                builder.makeIntConstant(-int32_t(tile_width_half)))));
  }

  // Get the linear tile index within the source texture.
  spv::Id source_tile_index = builder.createBinOp(
      spv::OpISub, type_uint, edram_tile_index_non_wrapped,
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, offsets_constant,
                          const_edram_base_tiles_bits_plus_1,
                          builder.makeUintConstant(xenos::kEdramBaseTilesBits)));
  // Split the linear tile index in the source texture into X and Y in tiles.
  spv::Id source_pitch_tiles =
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, pitches_constant,
                          const_edram_pitch_tiles_bits, const_edram_pitch_tiles_bits);
  spv::Id source_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, source_tile_index, source_pitch_tiles);
  spv::Id source_tile_index_x =
      builder.createBinOp(spv::OpUMod, type_uint, source_tile_index, source_pitch_tiles);
  // Combine the source tile offset and the sample index within the tile.
  spv::Id source_sample_x = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint, const_tile_width, source_tile_index_x),
      tile_sample_x);
  spv::Id source_sample_y = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint, const_tile_height, source_tile_index_y),
      tile_sample_y);
  // Get the source pixel coordinate and the sample index within the pixel.
  spv::Id source_pixel_x = source_sample_x, source_pixel_y = source_sample_y;
  spv::Id source_sample_id = spv::NoResult;
  if (source_is_multisampled) {
    spv::Id const_uint_1 = builder.makeUintConstant(1);
    source_pixel_y =
        builder.createBinOp(spv::OpShiftRightLogical, type_uint, source_sample_y, const_uint_1);
    if (key.msaa_samples >= xenos::MsaaSamples::k4X) {
      source_pixel_x =
          builder.createBinOp(spv::OpShiftRightLogical, type_uint, source_sample_x, const_uint_1);
      // 4x MSAA source texture sample index - bit 0 for horizontal, bit 1 for
      // vertical.
      source_sample_id = builder.createQuadOp(
          spv::OpBitFieldInsert, type_uint,
          builder.createBinOp(spv::OpBitwiseAnd, type_uint, source_sample_x, const_uint_1),
          source_sample_y, const_uint_1, const_uint_1);
    } else {
      // 2x MSAA source texture sample index - convert from the guest to
      // the Vulkan standard sample locations.
      source_sample_id = builder.createTriOp(
          spv::OpSelect, type_uint,
          builder.createBinOp(
              spv::OpINotEqual, builder.makeBoolType(),
              builder.createBinOp(spv::OpBitwiseAnd, type_uint, source_sample_y, const_uint_1),
              const_uint_0),
          builder.makeUintConstant(
              draw_util::GetD3D10SampleIndexForGuest2xMSAA(1, msaa_2x_attachments_supported_)),
          builder.makeUintConstant(
              draw_util::GetD3D10SampleIndexForGuest2xMSAA(0, msaa_2x_attachments_supported_)));
    }
  }

  // Load the source, and pack the value into one or two 32-bit integers.
  spv::Id packed[2] = {};
  spv::Builder::TextureParameters source_texture_parameters = {};
  source_texture_parameters.sampler = builder.createLoad(source_texture, spv::NoPrecision);
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.createUnaryOp(spv::OpBitcast, type_int, source_pixel_x));
  id_vector_temp.push_back(builder.createUnaryOp(spv::OpBitcast, type_int, source_pixel_y));
  source_texture_parameters.coords = builder.createCompositeConstruct(type_int2, id_vector_temp);
  if (source_is_multisampled) {
    source_texture_parameters.sample =
        builder.createUnaryOp(spv::OpBitcast, type_int, source_sample_id);
  } else {
    source_texture_parameters.lod = builder.makeIntConstant(0);
  }
  spv::Id source_vec4 = builder.createTextureCall(
      spv::NoPrecision, builder.makeVectorType(source_component_type, 4), false, true, false, false,
      false, source_texture_parameters, spv::ImageOperandsMaskNone);
  const bool source_color_16_is_float =
      !key.is_depth && IsColor16FormatFloatLike(key.GetColorFormat());
  spv::Id const_uint_16 = builder.makeUintConstant(16);
  spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
  spv::Id const_float_1 = builder.makeFloatConstant(1.0f);
  spv::Id const_float_minus_1 = builder.makeFloatConstant(-1.0f);
  spv::Id const_float_32767 = builder.makeFloatConstant(32767.0f);
  auto LinearToPWLGamma = [&](spv::Id linear, bool linear_pre_saturated) -> spv::Id {
    if (!linear_pre_saturated) {
      linear = builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                            linear, const_float_0, const_float_1);
    }
    spv::Id is_piece_at_least_3 =
        builder.createBinOp(spv::OpFOrdGreaterThanEqual, builder.makeBoolType(), linear,
                            builder.makeFloatConstant(512.0f / 1023.0f));
    spv::Id scale_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                               builder.makeFloatConstant(1023.0f / 8.0f),
                                               builder.makeFloatConstant(1023.0f / 4.0f));
    spv::Id offset_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                                builder.makeFloatConstant(128.0f / 255.0f),
                                                builder.makeFloatConstant(64.0f / 255.0f));
    spv::Id is_piece_at_least_1 =
        builder.createBinOp(spv::OpFOrdGreaterThanEqual, builder.makeBoolType(), linear,
                            builder.makeFloatConstant(64.0f / 1023.0f));
    spv::Id scale_1_or_0 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                                               builder.makeFloatConstant(1023.0f / 2.0f),
                                               builder.makeFloatConstant(1023.0f));
    spv::Id offset_1_or_0 =
        builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                            builder.makeFloatConstant(32.0f / 255.0f), const_float_0);
    spv::Id is_piece_at_least_2 =
        builder.createBinOp(spv::OpFOrdGreaterThanEqual, builder.makeBoolType(), linear,
                            builder.makeFloatConstant(128.0f / 1023.0f));
    spv::Id scale = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                        scale_3_or_2, scale_1_or_0);
    spv::Id offset = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                         offset_3_or_2, offset_1_or_0);
    return builder.createBinOp(
        spv::OpFAdd, type_float,
        builder.createBinOp(spv::OpFMul, type_float,
                            builder.createUnaryBuiltinCall(
                                type_float, ext_inst_glsl_std_450, GLSLstd450Trunc,
                                builder.createBinOp(spv::OpFMul, type_float, linear, scale)),
                            builder.makeFloatConstant(1.0f / 255.0f)),
        offset);
  };
  auto PackDumpSource16ComponentToUint = [&](spv::Id component) -> spv::Id {
    if (source_is_uint) {
      return component;
    }
    if (source_color_16_is_float) {
      id_vector_temp.clear();
      id_vector_temp.push_back(component);
      id_vector_temp.push_back(const_float_0);
      spv::Id packed_half = builder.createUnaryBuiltinCall(
          type_uint, ext_inst_glsl_std_450, GLSLstd450PackHalf2x16,
          builder.createCompositeConstruct(builder.makeVectorType(type_float, 2), id_vector_temp));
      return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed_half, const_uint_0,
                                 const_uint_16);
    }
    spv::Id component_clamped =
        builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp, component,
                                     const_float_minus_1, const_float_1);
    spv::Id component_rounded = builder.createUnaryBuiltinCall(
        type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
        builder.createBinOp(spv::OpFMul, type_float, component_clamped, const_float_32767));
    spv::Id component_snorm =
        builder.createUnaryOp(spv::OpConvertFToS, type_int, component_rounded);
    return builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                               builder.createUnaryOp(spv::OpBitcast, type_uint, component_snorm),
                               const_uint_0, const_uint_16);
  };
  auto PackDumpSource16PairToUint32 = [&](spv::Id component_0, spv::Id component_1) -> spv::Id {
    return builder.createQuadOp(
        spv::OpBitFieldInsert, type_uint, PackDumpSource16ComponentToUint(component_0),
        PackDumpSource16ComponentToUint(component_1), const_uint_16, const_uint_16);
  };
  if (key.is_depth) {
    source_texture_parameters.sampler =
        builder.createLoad(source_stencil_texture, spv::NoPrecision);
    spv::Id source_stencil = builder.createCompositeExtract(
        builder.createTextureCall(spv::NoPrecision, builder.makeVectorType(type_uint, 4), false,
                                  true, false, false, false, source_texture_parameters,
                                  spv::ImageOperandsMaskNone),
        type_uint, 0);
    spv::Id source_depth32 = builder.createCompositeExtract(source_vec4, type_float, 0);
    switch (key.GetDepthFormat()) {
      case xenos::DepthRenderTargetFormat::kD24S8: {
        // Round to the nearest even integer. This seems to be the correct
        // conversion, adding +0.5 and rounding towards zero results in red
        // instead of black in the 4D5307E6 clear shader.
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createUnaryBuiltinCall(
                type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                builder.createBinOp(spv::OpFMul, type_float, source_depth32,
                                    builder.makeFloatConstant(float(0xFFFFFF)))));
      } break;
      case xenos::DepthRenderTargetFormat::kD24FS8: {
        packed[0] = SpirvShaderTranslator::PreClampedDepthTo20e4(
            builder, source_depth32, depth_float24_round(), true, ext_inst_glsl_std_450);
      } break;
    }
    packed[0] = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, source_stencil, packed[0],
                                     builder.makeUintConstant(8), builder.makeUintConstant(24));
  } else {
    switch (key.GetColorFormat()) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
        if (gamma_render_target_as_unorm16_) {
          // 8_8_8_8_GAMMA is represented by linear stored in
          // R16G16B16A16_UNORM.
          id_vector_temp.clear();
          for (uint32_t i = 0; i < 3; ++i) {
            id_vector_temp.push_back(
                LinearToPWLGamma(builder.createCompositeExtract(source_vec4, type_float, i), true));
          }
        }
      }
        [[fallthrough]];
      case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
        spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
        spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
        spv::Id color_0 = builder.createCompositeExtract(source_vec4, type_float, 0);
        if (key.GetColorFormat() == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
            gamma_render_target_as_unorm16_) {
          color_0 = id_vector_temp[0];
        }
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createBinOp(spv::OpFAdd, type_float,
                                builder.createBinOp(spv::OpFMul, type_float, color_0, unorm_scale),
                                unorm_round_offset));
        spv::Id component_width = builder.makeUintConstant(8);
        for (uint32_t i = 1; i < 4; ++i) {
          spv::Id color_i = builder.createCompositeExtract(source_vec4, type_float, i);
          if (key.GetColorFormat() == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
              gamma_render_target_as_unorm16_ && i < 3) {
            color_i = id_vector_temp[i];
          }
          packed[0] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              builder.createUnaryOp(spv::OpConvertFToU, type_uint,
                                    builder.createBinOp(spv::OpFAdd, type_float,
                                                        builder.createBinOp(spv::OpFMul, type_float,
                                                                            color_i, unorm_scale),
                                                        unorm_round_offset)),
              builder.makeUintConstant(8 * i), component_width);
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
        spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
        spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createBinOp(
                spv::OpFAdd, type_float,
                builder.createBinOp(spv::OpFMul, type_float,
                                    builder.createCompositeExtract(source_vec4, type_float, 0),
                                    unorm_scale_rgb),
                unorm_round_offset));
        spv::Id width_rgb = builder.makeUintConstant(10);
        spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
        spv::Id width_a = builder.makeUintConstant(2);
        for (uint32_t i = 1; i < 4; ++i) {
          packed[0] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              builder.createUnaryOp(
                  spv::OpConvertFToU, type_uint,
                  builder.createBinOp(spv::OpFAdd, type_float,
                                      builder.createBinOp(spv::OpFMul, type_float,
                                                          builder.createCompositeExtract(
                                                              source_vec4, type_float, i),
                                                          i == 3 ? unorm_scale_a : unorm_scale_rgb),
                                      unorm_round_offset)),
              builder.makeUintConstant(10 * i), i == 3 ? width_a : width_rgb);
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
        // Float16 has a wider range for both color and alpha, also NaNs - clamp
        // and convert.
        packed[0] = SpirvShaderTranslator::UnclampedFloat32To7e3(
            builder, builder.createCompositeExtract(source_vec4, type_float, 0),
            ext_inst_glsl_std_450);
        spv::Id width_rgb = builder.makeUintConstant(10);
        for (uint32_t i = 1; i < 3; ++i) {
          packed[0] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              SpirvShaderTranslator::UnclampedFloat32To7e3(
                  builder, builder.createCompositeExtract(source_vec4, type_float, i),
                  ext_inst_glsl_std_450),
              builder.makeUintConstant(10 * i), width_rgb);
        }
        // Saturate and convert the alpha.
        spv::Id alpha_saturated = builder.createTriBuiltinCall(
            type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
            builder.createCompositeExtract(source_vec4, type_float, 3),
            builder.makeFloatConstant(0.0f), builder.makeFloatConstant(1.0f));
        packed[0] = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, packed[0],
            builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, alpha_saturated,
                                                        builder.makeFloatConstant(3.0f)),
                                    builder.makeFloatConstant(0.5f))),
            builder.makeUintConstant(30), builder.makeUintConstant(2));
      } break;
      case xenos::ColorRenderTargetFormat::k_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
        for (uint32_t i = 0; i <= uint32_t(format_is_64bpp); ++i) {
          packed[i] = PackDumpSource16PairToUint32(
              builder.createCompositeExtract(source_vec4, source_component_type, 2 * i),
              builder.createCompositeExtract(source_vec4, source_component_type, 2 * i + 1));
        }
      } break;
      // Float32 is transferred as uint32 to preserve NaN encodings. However,
      // multisampled sampled image support is optional in Vulkan.
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
        for (uint32_t i = 0; i <= uint32_t(format_is_64bpp); ++i) {
          spv::Id& packed_ref = packed[i];
          packed_ref = builder.createCompositeExtract(source_vec4, source_component_type, i);
          if (!source_is_uint) {
            packed_ref = builder.createUnaryOp(spv::OpBitcast, type_uint, packed_ref);
          }
        }
      } break;
    }
  }

  // Write the packed value to the EDRAM buffer.
  spv::Id store_value = packed[0];
  if (format_is_64bpp) {
    id_vector_temp.clear();
    id_vector_temp.push_back(packed[0]);
    id_vector_temp.push_back(packed[1]);
    store_value = builder.createCompositeConstruct(type_uint2, id_vector_temp);
  }
  id_vector_temp.clear();
  // The only SSBO structure member.
  id_vector_temp.push_back(builder.makeIntConstant(0));
  id_vector_temp.push_back(builder.createUnaryOp(spv::OpBitcast, type_int, edram_sample_address));
  // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // Uniform.
  builder.createStore(store_value, builder.createAccessChain(spv::StorageClassUniform, edram_buffer,
                                                             id_vector_temp));

  // End the main function and make it the entry point.
  builder.leaveFunction();
  builder.addExecutionMode(main_function, spv::ExecutionModeLocalSize, kDumpSamplesPerGroupX,
                           kDumpSamplesPerGroupY, 1);
  spv::Instruction* entry_point =
      builder.addEntryPoint(spv::ExecutionModelGLCompute, main_function, "main");
  // Bindings only need to be added to the entry point's interface starting with
  // SPIR-V 1.4 - emitting 1.0 here, so only inputs / outputs.
  entry_point->addIdOperand(input_global_invocation_id);

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);

  // Create the pipeline, and store the handle even if creation fails not to try
  // to create it again later.
  VkPipeline pipeline = ui::vulkan::util::CreateComputePipeline(
      command_processor_.GetVulkanDevice(),
      key.is_depth ? dump_pipeline_layout_depth_ : dump_pipeline_layout_color_,
      reinterpret_cast<const uint32_t*>(shader_code.data()), sizeof(uint32_t) * shader_code.size());
  if (pipeline == VK_NULL_HANDLE) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create a render target dumping "
        "pipeline for {}-sample render targets with format {}",
        UINT32_C(1) << uint32_t(key.msaa_samples),
        key.is_depth ? xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat())
                     : xenos::GetColorRenderTargetFormatName(key.GetColorFormat()));
  }
  dump_pipelines_.emplace(key, pipeline);
  return pipeline;
}

VkPipeline VulkanRenderTargetCache::GetDirectResolvePipeline(DirectResolvePipelineKey key) {
  auto pipeline_it = direct_resolve_pipelines_.find(key);
  if (pipeline_it != direct_resolve_pipelines_.end()) {
    return pipeline_it->second;
  }
  VkPipeline pipeline = VK_NULL_HANDLE;
  // Until dedicated direct host RT -> shared memory shaders are added, reuse
  // the resolve copy pipelines to keep all resolve shader modes wired for the
  // direct preflight path.
  size_t copy_shader_index = size_t(key.copy_shader);
  if (copy_shader_index < size_t(draw_util::ResolveCopyShaderIndex::kCount)) {
    pipeline = resolve_copy_pipelines_[copy_shader_index];
  }
  direct_resolve_pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool VulkanRenderTargetCache::TryResolveCopyDirectly(const draw_util::ResolveInfo& resolve_info,
                                                     draw_util::ResolveCopyShaderIndex copy_shader,
                                                     bool draw_resolution_scaled) {
  ++direct_resolve_attempt_count_;
  (void)copy_shader;
  (void)draw_resolution_scaled;
  if (direct_resolve_pipeline_layout_color_ == VK_NULL_HANDLE ||
      direct_resolve_pipeline_layout_depth_ == VK_NULL_HANDLE) {
    return false;
  }

  uint32_t dump_base;
  uint32_t dump_row_length_used;
  uint32_t dump_rows;
  uint32_t dump_pitch;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
  GetResolveCopyDispatchesToDump(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                                 dump_rectangles_, direct_resolve_dispatches_);
  if (direct_resolve_dispatches_.empty()) {
    return false;
  }

  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    const auto* render_target = static_cast<const VulkanRenderTarget*>(rectangle.render_target);
    if (render_target == nullptr) {
      return false;
    }
    DumpPipelineKey dump_pipeline_key;
    dump_pipeline_key.msaa_samples = render_target->key().msaa_samples;
    dump_pipeline_key.resource_format = render_target->key().resource_format;
    dump_pipeline_key.is_depth = render_target->key().is_depth;
    if (GetDumpPipeline(dump_pipeline_key) == VK_NULL_HANDLE) {
      return false;
    }
    DirectResolvePipelineKey direct_pipeline_key;
    direct_pipeline_key.dump_pipeline_key = dump_pipeline_key;
    direct_pipeline_key.copy_shader = copy_shader;
    direct_pipeline_key.draw_resolution_scaled = draw_resolution_scaled;
    if (GetDirectResolvePipeline(direct_pipeline_key) == VK_NULL_HANDLE) {
      return false;
    }
  }

  // Dedicated direct resolve dispatches are staged behind the same preflight;
  // keep using the existing dump path until source-image direct shaders land.
  return DumpRenderTargets(dump_base, dump_row_length_used, dump_rows, dump_pitch);
}

bool VulkanRenderTargetCache::DumpRenderTargets(uint32_t dump_base, uint32_t dump_row_length_used,
                                                uint32_t dump_rows, uint32_t dump_pitch) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                                 dump_rectangles_);
  if (dump_rectangles_.empty()) {
    return true;
  }

  // Clear previously set temporary indices.
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    static_cast<VulkanRenderTarget*>(rectangle.render_target)->SetTemporarySortIndex(UINT32_MAX);
  }
  // Gather all needed barriers and info needed to sort the invocations.
  UseEdramBuffer(EdramBufferUsage::kComputeWrite);
  dump_invocations_.clear();
  dump_invocations_.reserve(dump_rectangles_.size());
  uint32_t rt_sort_index = 0;
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = vulkan_rt.key();
    command_processor_.PushImageMemoryBarrier(
        vulkan_rt.image(),
        ui::vulkan::util::InitializeSubresourceRange(
            rt_key.is_depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                            : VK_IMAGE_ASPECT_COLOR_BIT),
        vulkan_rt.current_stage_mask(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        vulkan_rt.current_access_mask(), VK_ACCESS_SHADER_READ_BIT, vulkan_rt.current_layout(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vulkan_rt.SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (vulkan_rt.temporary_sort_index() == UINT32_MAX) {
      vulkan_rt.SetTemporarySortIndex(rt_sort_index++);
    }
    DumpPipelineKey pipeline_key;
    pipeline_key.msaa_samples = rt_key.msaa_samples;
    pipeline_key.resource_format = rt_key.resource_format;
    pipeline_key.is_depth = rt_key.is_depth;
    dump_invocations_.emplace_back(rectangle, pipeline_key);
  }

  // Sort the invocations to reduce context and binding switches.
  std::sort(dump_invocations_.begin(), dump_invocations_.end());

  // Dump the render targets.
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  bool edram_buffer_bound = false;
  VkDescriptorSet last_source_descriptor_set = VK_NULL_HANDLE;
  DumpPitches last_pitches;
  DumpOffsets last_offsets;
  bool pitches_bound = false, offsets_bound = false;
  bool all_pipelines_available = true;
  for (const DumpInvocation& invocation : dump_invocations_) {
    const ResolveCopyDumpRectangle& rectangle = invocation.rectangle;
    auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = vulkan_rt.key();
    DumpPipelineKey pipeline_key = invocation.pipeline_key;
    VkPipeline pipeline = GetDumpPipeline(pipeline_key);
    if (!pipeline) {
      all_pipelines_available = false;
      continue;
    }
    command_processor_.BindExternalComputePipeline(pipeline);

    VkPipelineLayout pipeline_layout =
        rt_key.is_depth ? dump_pipeline_layout_depth_ : dump_pipeline_layout_color_;

    // Only need to bind the EDRAM buffer once (relying on pipeline layout
    // compatibility).
    if (!edram_buffer_bound) {
      edram_buffer_bound = true;
      command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                             kDumpDescriptorSetEdram, 1,
                                             &edram_storage_buffer_descriptor_set_, 0, nullptr);
    }

    VkDescriptorSet source_descriptor_set = vulkan_rt.GetDescriptorSetTransferSource();
    if (last_source_descriptor_set != source_descriptor_set) {
      last_source_descriptor_set = source_descriptor_set;
      command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                             kDumpDescriptorSetSource, 1, &source_descriptor_set, 0,
                                             nullptr);
    }

    DumpPitches pitches;
    pitches.dest_pitch = dump_pitch;
    pitches.source_pitch = rt_key.GetPitchTiles();
    if (last_pitches != pitches) {
      last_pitches = pitches;
      pitches_bound = false;
    }
    if (!pitches_bound) {
      pitches_bound = true;
      command_buffer.CmdVkPushConstants(pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                        sizeof(uint32_t) * kDumpPushConstantPitches,
                                        sizeof(last_pitches), &last_pitches);
    }

    DumpOffsets offsets;
    offsets.source_base_tiles = rt_key.base_tiles;
    ResolveCopyDumpRectangle::Dispatch dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count = rectangle.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    for (uint32_t i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];
      offsets.dispatch_first_tile = dump_base + dispatch.offset;
      if (last_offsets != offsets) {
        last_offsets = offsets;
        offsets_bound = false;
      }
      if (!offsets_bound) {
        offsets_bound = true;
        command_buffer.CmdVkPushConstants(pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                          sizeof(uint32_t) * kDumpPushConstantOffsets,
                                          sizeof(last_offsets), &last_offsets);
      }
      command_processor_.SubmitBarriers(true);
      command_buffer.CmdVkDispatch(
          (draw_resolution_scale_x() *
               (xenos::kEdramTileWidthSamples >> uint32_t(rt_key.Is64bpp())) *
               dispatch.width_tiles +
           (kDumpSamplesPerGroupX - 1)) /
              kDumpSamplesPerGroupX,
          (draw_resolution_scale_y() * xenos::kEdramTileHeightSamples * dispatch.height_tiles +
           (kDumpSamplesPerGroupY - 1)) /
              kDumpSamplesPerGroupY,
          1);
    }
    MarkEdramBufferModified();
  }
  return all_pipelines_available;
}

namespace {
// The XDK's internal clear vertex shader (D3DDevice_Clear): screen-space xyz
// from its only vertex stream straight to the position, and the clear color.
constexpr uint64_t kXdkClearVertexShaderHash = 0x0A6D1DD7767FDF27;
// Its pixel shader for color clears: the interpolated clear color to oC0.
constexpr uint64_t kXdkClearPixelShaderHash = 0x2E372EA28CC404B7;
}  // namespace

void VulkanRenderTargetCache::FlushDeferredTransfers() {
  if (!deferred_transfer_targets_) {
    return;
  }
  std::array<std::vector<Transfer>, 1 + xenos::kMaxColorRenderTargets> cut_transfers;
  RenderTarget* cut_render_targets[1 + xenos::kMaxColorRenderTargets] = {};
  uint32_t dropped_count = 0;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!(deferred_transfer_targets_ & (uint32_t(1) << i))) {
      continue;
    }
    RenderTarget* render_target = deferred_transfer_bindings_[i];
    RenderTargetKey key = render_target->key();
    for (const Transfer& transfer : deferred_transfers_[i]) {
      Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
      if (transfer.GetRectangles(key.base_tiles, key.GetPitchTiles(), key.msaa_samples,
                                 key.Is64bpp(), rectangles, &deferred_transfer_cutout_)) {
        cut_transfers[i].push_back(transfer);
      } else {
        ++dropped_count;
      }
    }
    deferred_transfers_[i].clear();
    if (!cut_transfers[i].empty()) {
      cut_render_targets[i] = render_target;
    }
  }
  deferred_transfer_targets_ = 0;
  if (RtDebugLogActive()) {
    REXGPU_INFO("[rt-debug] held back transfers performed around ({},{}) {}x{}, {} dropped",
                deferred_transfer_cutout_.x_pixels, deferred_transfer_cutout_.y_pixels,
                deferred_transfer_cutout_.width_pixels, deferred_transfer_cutout_.height_pixels,
                dropped_count);
  }
  PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets, cut_render_targets,
                                   cut_transfers.data(), nullptr, &deferred_transfer_cutout_);
}

bool VulkanRenderTargetCache::MergeTransferCutout(Transfer::Rectangle& accumulated,
                                                  const Transfer::Rectangle& rectangle) {
  uint32_t ax0 = accumulated.x_pixels, ax1 = ax0 + accumulated.width_pixels;
  uint32_t ay0 = accumulated.y_pixels, ay1 = ay0 + accumulated.height_pixels;
  uint32_t bx0 = rectangle.x_pixels, bx1 = bx0 + rectangle.width_pixels;
  uint32_t by0 = rectangle.y_pixels, by1 = by0 + rectangle.height_pixels;
  if (bx0 >= ax0 && bx1 <= ax1 && by0 >= ay0 && by1 <= ay1) {
    return true;
  }
  if ((ax0 >= bx0 && ax1 <= bx1 && ay0 >= by0 && ay1 <= by1) ||
      (ay0 == by0 && ay1 == by1 && bx0 <= ax1 && bx1 >= ax0) ||
      (ax0 == bx0 && ax1 == bx1 && by0 <= ay1 && by1 >= ay0)) {
    uint32_t x0 = std::min(ax0, bx0), y0 = std::min(ay0, by0);
    accumulated.x_pixels = x0;
    accumulated.y_pixels = y0;
    accumulated.width_pixels = std::max(ax1, bx1) - x0;
    accumulated.height_pixels = std::max(ay1, by1) - y0;
    return true;
  }
  return false;
}

void VulkanRenderTargetCache::UpdateUniformStencil(VulkanRenderTarget& depth_render_target,
                                                   reg::RB_DEPTHCONTROL normalized_depth_control,
                                                   uint32_t normalized_color_mask,
                                                   const Shader& vertex_shader) {
  VulkanRenderTarget::UniformStencil& uniform = depth_render_target.uniform_stencil();
  const RegisterFile& regs = register_file();
  auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>();
  reg::RB_STENCILREFMASK stencil_ref_mask_bf;
  stencil_ref_mask_bf.value = regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
  Transfer::Rectangle rectangle;
  bool exact_edges;
  if (GetDrawOverwrittenRenderTargets(normalized_depth_control, normalized_color_mask,
                                      vertex_shader, rectangle, &exact_edges, 1) &&
      exact_edges) {
    // The stencil in the rectangle is replaced with the reference value.
    uint32_t value = stencil_ref_mask.stencilref;
    if (normalized_depth_control.backface_enable && stencil_ref_mask_bf.stencilref != value) {
      uniform.generation = 0;
      return;
    }
    if (uniform.generation == uniform_stencil_generation_ && uniform.value == value &&
        MergeTransferCutout(uniform.rectangle, rectangle)) {
      return;
    }
    uniform.rectangle = rectangle;
    uniform.value = value;
    uniform.generation = uniform_stencil_generation_;
    return;
  }
  if (!normalized_depth_control.stencil_enable) {
    return;
  }
  bool writes_stencil = stencil_ref_mask.stencilwritemask &&
                        (normalized_depth_control.stencilfail != xenos::StencilOp::kKeep ||
                         normalized_depth_control.stencilzfail != xenos::StencilOp::kKeep ||
                         normalized_depth_control.stencilzpass != xenos::StencilOp::kKeep);
  if (normalized_depth_control.backface_enable) {
    writes_stencil |= stencil_ref_mask_bf.stencilwritemask &&
                      (normalized_depth_control.stencilfail_bf != xenos::StencilOp::kKeep ||
                       normalized_depth_control.stencilzfail_bf != xenos::StencilOp::kKeep ||
                       normalized_depth_control.stencilzpass_bf != xenos::StencilOp::kKeep);
  }
  if (writes_stencil) {
    // Anywhere the draw covers.
    uniform.generation = 0;
  }
}

void VulkanRenderTargetCache::ForgetUniformStencilWrittenByTransfers(
    VulkanRenderTarget& depth_render_target, const std::vector<Transfer>& transfers,
    const Transfer::Rectangle* cutout) {
  VulkanRenderTarget::UniformStencil& uniform = depth_render_target.uniform_stencil();
  if (uniform.generation != uniform_stencil_generation_) {
    return;
  }
  const Transfer::Rectangle& known = uniform.rectangle;
  RenderTargetKey key = depth_render_target.key();
  for (const Transfer& transfer : transfers) {
    Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
    uint32_t rectangle_count = transfer.GetRectangles(
        key.base_tiles, key.GetPitchTiles(), key.msaa_samples, key.Is64bpp(), rectangles, cutout);
    for (uint32_t i = 0; i < rectangle_count; ++i) {
      const Transfer::Rectangle& written = rectangles[i];
      if (written.x_pixels < known.x_pixels + known.width_pixels &&
          known.x_pixels < written.x_pixels + written.width_pixels &&
          written.y_pixels < known.y_pixels + known.height_pixels &&
          known.y_pixels < written.y_pixels + written.height_pixels) {
        uniform.generation = 0;
        return;
      }
    }
  }
}

bool VulkanRenderTargetCache::GetUniformStencil(VulkanRenderTarget& depth_render_target,
                                                uint32_t x0, uint32_t y0, uint32_t x1,
                                                uint32_t y1, uint32_t& value_out) {
  if (!REXCVAR_GET(native_resolve_uniform_stencil)) {
    return false;
  }
  const VulkanRenderTarget::UniformStencil& uniform = depth_render_target.uniform_stencil();
  if (uniform.generation != uniform_stencil_generation_ || x0 < uniform.rectangle.x_pixels ||
      y0 < uniform.rectangle.y_pixels ||
      x1 > uniform.rectangle.x_pixels + uniform.rectangle.width_pixels ||
      y1 > uniform.rectangle.y_pixels + uniform.rectangle.height_pixels) {
    return false;
  }
  value_out = uniform.value;
  return true;
}

uint32_t VulkanRenderTargetCache::GetDrawOverwrittenRenderTargets(
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
    const Shader& vertex_shader, Transfer::Rectangle& rectangle_out, bool* exact_edges_out,
    uint32_t candidate_targets, bool depth_without_stencil) const {
  if (exact_edges_out) {
    *exact_edges_out = false;
  }
  // Coverage alone doesn't prove an overwrite if depth or stencil can reject
  // fragments. Those pixels still need their previous color, depth and stencil,
  // even with opaque blending and full write masks.
  if ((normalized_depth_control.z_enable &&
       normalized_depth_control.zfunc != xenos::CompareFunction::kAlways) ||
      (normalized_depth_control.stencil_enable &&
       (normalized_depth_control.stencilfunc != xenos::CompareFunction::kAlways ||
        (normalized_depth_control.backface_enable &&
         normalized_depth_control.stencilfunc_bf != xenos::CompareFunction::kAlways)))) {
    return 0;
  }
  const RegisterFile& regs = register_file();
  // The XDK's clear vertex shader takes the screen-space positions straight
  // from its only vertex stream. Any other vertex shader the CPU interpreter
  // can run (post-processing passes, for instance) is run for the 3 vertices.
  bool xdk_clear_vs = vertex_shader.ucode_data_hash() == kXdkClearVertexShaderHash;
  static const bool cross_check = std::getenv("REX_RT_OVERWRITE_CROSSCHECK") != nullptr;
  if (!xdk_clear_vs && !REXCVAR_GET(native_rt_cpu_vs_overwrite_proofs)) {
    return 0;
  }
  auto draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (draw_initiator.prim_type != xenos::PrimitiveType::kRectangleList ||
      draw_initiator.num_indices != 3 ||
      draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    return 0;
  }
  // The render targets the draw replaces wherever it covers, checked before
  // the coverage, which may need the vertex shader to be run.
  uint32_t overwritten = 0;
  // Depth and stencil both fully replaced (or only the depth).
  if (depth_without_stencil) {
    if (normalized_depth_control.z_enable && normalized_depth_control.z_write_enable &&
        normalized_depth_control.zfunc == xenos::CompareFunction::kAlways) {
      overwritten |= 1;
    }
  } else if (normalized_depth_control.z_enable && normalized_depth_control.z_write_enable &&
      normalized_depth_control.zfunc == xenos::CompareFunction::kAlways &&
      normalized_depth_control.stencil_enable &&
      normalized_depth_control.stencilfunc == xenos::CompareFunction::kAlways &&
      normalized_depth_control.stencilzpass == xenos::StencilOp::kReplace &&
      regs.Get<reg::RB_STENCILREFMASK>().stencilwritemask == 0xFF) {
    bool back_replaced = true;
    if (normalized_depth_control.backface_enable) {
      reg::RB_STENCILREFMASK stencil_ref_mask_bf;
      stencil_ref_mask_bf.value = regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
      back_replaced = normalized_depth_control.stencilfunc_bf == xenos::CompareFunction::kAlways &&
                      normalized_depth_control.stencilzpass_bf == xenos::StencilOp::kReplace &&
                      stencil_ref_mask_bf.stencilwritemask == 0xFF;
    }
    if (back_replaced) {
      overwritten |= 1;
    }
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    // The normalized mask has components the format doesn't have set too.
    if (((normalized_color_mask >> (4 * i)) & 0b1111) != 0b1111) {
      continue;
    }
    auto blend_control = regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]);
    if (blend_control.color_srcblend != xenos::BlendFactor::kOne ||
        blend_control.color_destblend != xenos::BlendFactor::kZero ||
        blend_control.color_comb_fcn != xenos::BlendOp::kAdd ||
        blend_control.alpha_srcblend != xenos::BlendFactor::kOne ||
        blend_control.alpha_destblend != xenos::BlendFactor::kZero ||
        blend_control.alpha_comb_fcn != xenos::BlendOp::kAdd) {
      continue;
    }
    overwritten |= uint32_t(1) << (1 + i);
  }
  overwritten &= candidate_targets;
  if (!overwritten) {
    return 0;
  }
  uint32_t stride = 0;
  xenos::xe_gpu_vertex_fetch_t fetch = {};
  const float* vertices = nullptr;
  if (xdk_clear_vs) {
    const std::vector<Shader::VertexBinding>& bindings = vertex_shader.vertex_bindings();
    if (bindings.size() != 1 || bindings[0].stride_words < 3) {
      return 0;
    }
    stride = bindings[0].stride_words;
    fetch = regs.GetVertexFetch(bindings[0].fetch_constant);
    if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size < stride * 2 + 3) {
      return 0;
    }
    vertices = memory_.TranslatePhysical<const float*>(fetch.address * sizeof(uint32_t));
    if (!vertices) {
      return 0;
    }
  }

  auto sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (sc_mode_cntl.cull_front || sc_mode_cntl.cull_back ||
      sc_mode_cntl.poly_mode != xenos::PolygonModeEnable::kDisabled) {
    return 0;
  }
  auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  if (surface_info.msaa_samples != xenos::MsaaSamples::k1X) {
    uint32_t sample_mask = (uint32_t(1) << (uint32_t(1) << uint32_t(surface_info.msaa_samples))) - 1;
    if ((regs[XE_GPU_REG_PA_SC_AA_MASK] & sample_mask) != sample_mask) {
      return 0;
    }
  }
  const Shader* pixel_shader = command_processor_.active_pixel_shader();
  if (pixel_shader && (pixel_shader->kills_pixels() || pixel_shader->writes_depth())) {
    return 0;
  }
  auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  if (color_control.alpha_to_mask_enable ||
      (color_control.alpha_test_enable &&
       color_control.alpha_func != xenos::CompareFunction::kAlways)) {
    return 0;
  }

  auto vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto reg_float = [&regs](uint32_t index) {
    float value;
    std::memcpy(&value, &regs[index], sizeof(value));
    return value;
  };
  auto clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  bool clip_disable = clip_cntl.clip_disable;
  if (!clip_disable && clip_cntl.ucp_ena) {
    // User clip planes.
    return 0;
  }
  float half_pixel_offset =
      regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
  // REX_RT_OVERWRITE_CROSSCHECK: for XDK clears, also run the shader on the CPU
  // and log where the two disagree (validation of the interpreter path).
  float interpreted_positions[3][4];
  bool interpreted = false;
  if (!xdk_clear_vs || cross_check) {
    interpreted = draw_extent_estimator().GetAutoIndexedVertexPositions(vertex_shader, 3,
                                                                        interpreted_positions);
    if (!xdk_clear_vs && !interpreted) {
      return 0;
    }
  }
  int32_t x_fixed[3], y_fixed[3];
  for (uint32_t i = 0; i < 3; ++i) {
    float x, y;
    if (xdk_clear_vs) {
      const float* vertex = vertices + stride * i;
      x = xenos::GpuSwap(vertex[0], fetch.endian);
      y = xenos::GpuSwap(vertex[1], fetch.endian);
      float z = xenos::GpuSwap(vertex[2], fetch.endian);
      // w is 1. Anything that could be clipped makes the coverage unknown.
      if (!clip_disable && !(z >= 0.0f && z <= 1.0f)) {
        return 0;
      }
      if (cross_check && i == 0) {
        static std::atomic<uint32_t> checks{0};
        uint32_t n = ++checks;
        if (!(n & (n - 1))) {
          REXGPU_INFO("[rt-overwrite-check] XDK clear #{}: CPU interpreter {}", n,
                      interpreted ? "ran" : "could not run");
        }
      }
      if (interpreted) {
        const float* p = interpreted_positions[i];
        if (p[0] != x || p[1] != y || p[2] != z || p[3] != 1.0f) {
          REXGPU_INFO("[rt-overwrite-check] VS {:016X} vertex {}: memory ({},{},{}) CPU ({},{},{},{})",
                      vertex_shader.ucode_data_hash(), i, x, y, z, p[0], p[1], p[2], p[3]);
        }
      }
    } else {
      // The position as the host gets it: the shader may return 1/W, and Z/W
      // instead of Z (VTX_W0_FMT and VTX_Z_FMT, see the shader translator).
      const float* p = interpreted_positions[i];
      float w = vte_cntl.vtx_w0_fmt ? p[3] : 1.0f / p[3];
      float z = vte_cntl.vtx_z_fmt ? p[2] * w : p[2];
      // Anything that could be clipped makes the coverage unknown.
      if (!(w > 0.0f) || (!clip_disable && !(z >= 0.0f && z <= w))) {
        return 0;
      }
      x = p[0];
      y = p[1];
      if (!vte_cntl.vtx_xy_fmt) {
        x /= w;
        y /= w;
      }
    }
    if (vte_cntl.vport_x_scale_ena) {
      x *= reg_float(XE_GPU_REG_PA_CL_VPORT_XSCALE);
    }
    if (vte_cntl.vport_x_offset_ena) {
      x += reg_float(XE_GPU_REG_PA_CL_VPORT_XOFFSET);
    }
    if (vte_cntl.vport_y_scale_ena) {
      y *= reg_float(XE_GPU_REG_PA_CL_VPORT_YSCALE);
    }
    if (vte_cntl.vport_y_offset_ena) {
      y += reg_float(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
    }
    if (!std::isfinite(x) || !std::isfinite(y)) {
      return 0;
    }
    x_fixed[i] = ui::FloatToD3D11Fixed16p8(x + half_pixel_offset);
    y_fixed[i] = ui::FloatToD3D11Fixed16p8(y + half_pixel_offset);
  }
  if (RtDebugLogActive()) {
    REXGPU_INFO("[rt-debug] clear vertices (1/256 px): ({},{}) ({},{}) ({},{}) msaa={}",
                x_fixed[0], y_fixed[0], x_fixed[1], y_fixed[1], x_fixed[2], y_fixed[2],
                1u << uint32_t(surface_info.msaa_samples));
  }
  if (exact_edges_out) {
    // Edges on pixel boundaries: the same pixels (and all of their samples, or
    // none) are covered at any resolution scale.
    bool exact_edges = true;
    for (uint32_t i = 0; i < 3; ++i) {
      exact_edges &= !(x_fixed[i] & 0xFF) && !(y_fixed[i] & 0xFF);
    }
    *exact_edges_out = exact_edges;
  }
  // The rectangle covers the bounding box of the vertices only if they are
  // three distinct corners of an axis-aligned rectangle.
  int32_t x0 = std::min({x_fixed[0], x_fixed[1], x_fixed[2]});
  int32_t x1 = std::max({x_fixed[0], x_fixed[1], x_fixed[2]});
  int32_t y0 = std::min({y_fixed[0], y_fixed[1], y_fixed[2]});
  int32_t y1 = std::max({y_fixed[0], y_fixed[1], y_fixed[2]});
  for (uint32_t i = 0; i < 3; ++i) {
    if ((x_fixed[i] != x0 && x_fixed[i] != x1) || (y_fixed[i] != y0 && y_fixed[i] != y1)) {
      return 0;
    }
    for (uint32_t j = 0; j < i; ++j) {
      if (x_fixed[i] == x_fixed[j] && y_fixed[i] == y_fixed[j]) {
        return 0;
      }
    }
  }
  if (surface_info.msaa_samples == xenos::MsaaSamples::k1X) {
    // Pixel centers inside according to the top-left rule.
    x0 = (x0 + 127) >> 8;
    y0 = (y0 + 127) >> 8;
    x1 = (x1 + 127) >> 8;
    y1 = (y1 + 127) >> 8;
  } else {
    // Only pixels entirely inside have all their samples covered.
    x0 = (x0 + 255) >> 8;
    y0 = (y0 + 255) >> 8;
    x1 >>= 8;
    y1 >>= 8;
  }
  if (sc_mode_cntl.vtx_window_offset_enable) {
    auto window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    x0 += window_offset.window_x_offset;
    y0 += window_offset.window_y_offset;
    x1 += window_offset.window_x_offset;
    y1 += window_offset.window_y_offset;
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor, false);
  x0 = std::clamp(x0, int32_t(scissor.offset[0]), int32_t(scissor.offset[0] + scissor.extent[0]));
  x1 = std::clamp(x1, int32_t(scissor.offset[0]), int32_t(scissor.offset[0] + scissor.extent[0]));
  y0 = std::clamp(y0, int32_t(scissor.offset[1]), int32_t(scissor.offset[1] + scissor.extent[1]));
  y1 = std::clamp(y1, int32_t(scissor.offset[1]), int32_t(scissor.offset[1] + scissor.extent[1]));
  if (x0 < 0 || y0 < 0 || x0 >= x1 || y0 >= y1) {
    return 0;
  }

  if (overwritten && cross_check && !xdk_clear_vs) {
    static std::mutex proven_mutex;
    static std::unordered_set<uint64_t> proven_shaders;
    std::lock_guard<std::mutex> lock(proven_mutex);
    if (proven_shaders.insert(vertex_shader.ucode_data_hash()).second) {
      REXGPU_INFO("[rt-overwrite-check] VS {:016X} proven to overwrite ({},{})-({},{}), targets {:X}",
                  vertex_shader.ucode_data_hash(), x0, y0, x1, y1, overwritten);
    }
  }
  if (overwritten) {
    // In render target pixels, which are samples when multisampled surfaces
    // are single-sampled on the host.
    uint32_t x_log2 = 0, y_log2 = 0;
    if (msaa_as_single_sample()) {
      x_log2 = uint32_t(surface_info.msaa_samples >= xenos::MsaaSamples::k4X);
      y_log2 = uint32_t(surface_info.msaa_samples >= xenos::MsaaSamples::k2X);
    }
    rectangle_out.x_pixels = uint32_t(x0) << x_log2;
    rectangle_out.y_pixels = uint32_t(y0) << y_log2;
    rectangle_out.width_pixels = uint32_t(x1 - x0) << x_log2;
    rectangle_out.height_pixels = uint32_t(y1 - y0) << y_log2;
  }
  return overwritten;
}

bool VulkanRenderTargetCache::ClearDrawAsAttachmentClear(
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
    const Shader& vertex_shader, const Shader* pixel_shader) {
  if (!native_rt_mode_ || GetPath() != Path::kHostRenderTargets ||
      !REXCVAR_GET(native_rt_clear_draws_as_clears) ||
      vertex_shader.ucode_data_hash() != kXdkClearVertexShaderHash ||
      (pixel_shader && pixel_shader->ucode_data_hash() != kXdkClearPixelShaderHash) ||
      command_processor_.IsHostOcclusionQueryActive() ||
      last_update_render_pass_key_.color_rts_use_transfer_formats ||
      !last_update_framebuffer_) {
    return false;
  }
  const RegisterFile& regs = register_file();
  RenderTarget* const* render_targets = last_update_accumulated_render_targets();

  // Everything the draw writes must be overwritten entirely in the rectangle.
  uint32_t written = 0;
  bool writes_stencil = false;
  if (render_targets[0]) {
    auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>();
    writes_stencil =
        normalized_depth_control.stencil_enable && stencil_ref_mask.stencilwritemask;
    if (normalized_depth_control.backface_enable) {
      reg::RB_STENCILREFMASK stencil_ref_mask_bf;
      stencil_ref_mask_bf.value = regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
      writes_stencil |=
          normalized_depth_control.stencil_enable && stencil_ref_mask_bf.stencilwritemask;
    }
    if ((normalized_depth_control.z_enable && normalized_depth_control.z_write_enable) ||
        writes_stencil) {
      written |= 1;
    }
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (render_targets[1 + i] && ((normalized_color_mask >> (4 * i)) & 0b1111)) {
      written |= uint32_t(1) << (1 + i);
    }
  }
  if (!written) {
    return false;
  }
  Transfer::Rectangle rectangle;
  // A draw that keeps the stencil only needs the depth replaced, and then
  // clears only the depth.
  if ((GetDrawOverwrittenRenderTargets(normalized_depth_control, normalized_color_mask,
                                       vertex_shader, rectangle, nullptr, written,
                                       !writes_stencil) &
       written) != written) {
    return false;
  }
  auto sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (sc_mode_cntl.poly_offset_front_enable || sc_mode_cntl.poly_offset_back_enable ||
      sc_mode_cntl.poly_offset_para_enable) {
    return false;
  }

  // The clear values, constant across the vertices: the clear vertex shader
  // takes xyz (w = 1) and the color from its only vertex stream, and the pixel
  // shader outputs the interpolated color.
  const std::vector<Shader::VertexBinding>& bindings = vertex_shader.vertex_bindings();
  if (bindings.size() != 1 || bindings[0].stride_words < 7) {
    return false;
  }
  uint32_t stride = bindings[0].stride_words;
  xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(bindings[0].fetch_constant);
  if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size < stride * 2 + 7) {
    return false;
  }
  const uint32_t* vertices =
      memory_.TranslatePhysical<const uint32_t*>(fetch.address * sizeof(uint32_t));
  if (!vertices) {
    return false;
  }
  uint32_t values[5];
  for (uint32_t i = 0; i < 3; ++i) {
    for (uint32_t j = 0; j < 5; ++j) {
      // z, then RGBA.
      uint32_t value = xenos::GpuSwap(vertices[stride * i + 2 + j], fetch.endian);
      if (!i) {
        values[j] = value;
      } else if (value != values[j]) {
        return false;
      }
    }
  }
  float z, color[4];
  std::memcpy(&z, &values[0], sizeof(float));
  std::memcpy(color, &values[1], sizeof(color));

  VkClearAttachment attachments[1 + xenos::kMaxColorRenderTargets];
  uint32_t attachment_count = 0;
  bool original_resolution = false, scaled = false;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!(written & (uint32_t(1) << i))) {
      continue;
    }
    RenderTargetKey key = render_targets[i]->key();
    (key.original_resolution ? original_resolution : scaled) = true;
    VkClearAttachment& attachment = attachments[attachment_count++];
    std::memset(&attachment, 0, sizeof(attachment));
    if (!i) {
      // The depth the draw writes: the shader passes z through (times 1 plus
      // 0 with these viewport parameters) and the viewport maps [0, 1] to
      // [0, z_max] - exact only for a power of two z_max. Only for a float
      // host depth buffer, which the clear value goes to unconverted.
      if (GetDepthVulkanFormat(key.GetDepthFormat()) != VK_FORMAT_D32_SFLOAT_S8_UINT) {
        return false;
      }
      draw_util::ViewportInfo viewport_info;
      const ui::vulkan::VulkanDevice::Properties& device_properties =
          command_processor_.GetVulkanDevice()->properties();
      draw_util::GetHostViewportInfo(
          regs, draw_resolution_scale_x(), draw_resolution_scale_y(), false,
          device_properties.maxViewportDimensions[0], device_properties.maxViewportDimensions[1],
          true, normalized_depth_control, depth_float24_convert_in_pixel_shader(), true, false,
          viewport_info);
      if (viewport_info.ndc_scale[2] != 1.0f || viewport_info.ndc_offset[2] != 0.0f ||
          viewport_info.z_min != 0.0f ||
          (viewport_info.z_max != 1.0f && viewport_info.z_max != 0.5f) || !(z >= 0.0f) ||
          !(z <= 1.0f) || (z == 0.0f && std::signbit(z))) {
        return false;
      }
      auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>();
      if (writes_stencil && normalized_depth_control.backface_enable) {
        reg::RB_STENCILREFMASK stencil_ref_mask_bf;
        stencil_ref_mask_bf.value = regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
        if (stencil_ref_mask_bf.stencilref != stencil_ref_mask.stencilref) {
          return false;
        }
      }
      attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
      if (writes_stencil) {
        attachment.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
      }
      attachment.clearValue.depthStencil.depth = z * viewport_info.z_max;
      attachment.clearValue.depthStencil.stencil = stencil_ref_mask.stencilref;
    } else {
      // Unorm formats only, which store the clamped color the same way from
      // a clear value as from the pixel shader, without an exponent bias.
      switch (key.GetColorFormat()) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
          break;
        default:
          return false;
      }
      if (regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i - 1])
              .color_exp_bias) {
        return false;
      }
      for (uint32_t j = 0; j < 4; ++j) {
        if (!std::isfinite(color[j])) {
          return false;
        }
        attachment.clearValue.color.float32[j] = color[j];
      }
      attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      // Subpass color attachments are indexed by the guest render target.
      attachment.colorAttachment = i - 1;
    }
  }
  if (original_resolution && scaled) {
    return false;
  }
  uint32_t scale_x = original_resolution ? 1 : draw_resolution_scale_x();
  uint32_t scale_y = original_resolution ? 1 : draw_resolution_scale_y();
  VkClearRect clear_rect;
  clear_rect.rect.offset.x = int32_t(rectangle.x_pixels * scale_x);
  clear_rect.rect.offset.y = int32_t(rectangle.y_pixels * scale_y);
  clear_rect.rect.extent.width = rectangle.width_pixels * scale_x;
  clear_rect.rect.extent.height = rectangle.height_pixels * scale_y;
  clear_rect.baseArrayLayer = 0;
  clear_rect.layerCount = 1;
  const VkExtent2D& host_extent = last_update_framebuffer_->host_extent;
  if (uint32_t(clear_rect.rect.offset.x) + clear_rect.rect.extent.width > host_extent.width ||
      uint32_t(clear_rect.rect.offset.y) + clear_rect.rect.extent.height > host_extent.height) {
    return false;
  }

  command_processor_.SubmitBarriersAndEnterRenderTargetCacheRenderPass(last_update_render_pass_,
                                                                      last_update_framebuffer_);
  command_processor_.deferred_command_buffer().CmdVkClearAttachments(attachment_count, attachments,
                                                                     1, &clear_rect);
  if (RtDebugLogActive()) {
    REXGPU_INFO("[rt-debug] clear draw done as a clear of targets {:#x} in ({},{}) {}x{}", written,
                rectangle.x_pixels, rectangle.y_pixels, rectangle.width_pixels,
                rectangle.height_pixels);
  }
  return true;
}

namespace {

constexpr char kNativeResolveVertexShaderSource[] = R"(#version 450
void main() {
  vec2 position = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Destination texel (x, y) is source render target pixel (x, y) plus the offset.
// The output is the raw texel bits, as the texture would load them from the
// memory the resolve writes.
constexpr char kNativeResolveFragmentShaderHeader[] = R"(
#extension GL_EXT_samplerless_texture_functions : require
layout(push_constant) uniform XeNativeResolveConstants {
  ivec2 xe_native_resolve_source_offset;
  uint xe_native_resolve_flags;
  uint xe_native_resolve_packing;
  uint xe_native_resolve_dest_base_dwords;
  uint xe_native_resolve_dest_pitch;
  uint xe_native_resolve_scale;
  uint xe_native_resolve_stencil_capture_origin;
  uint xe_native_resolve_stencil_capture_pitch_quads;
  uint xe_native_resolve_memory_base_dwords;
};
#if XE_NATIVE_RESOLVE_FLOAT_OUTPUT
layout(location = 0) out vec4 xe_native_resolve_output_float;
#else
layout(location = 0) out uvec4 xe_native_resolve_output;
#endif
layout(set = 1, binding = 0) buffer XeSharedMemory {
  uint data[];
} xe_shared_memory[1 << XE_SHARED_MEMORY_BINDING_COUNT_LOG2];
void XeSharedMemoryStore(uint address_dwords, uint value) {
#if XE_SHARED_MEMORY_BINDING_COUNT_LOG2 == 0
  xe_shared_memory[0].data[address_dwords] = value;
#else
  const uint binding_address_bits = 27u - uint(XE_SHARED_MEMORY_BINDING_COUNT_LOG2);
  uint binding_address = address_dwords & ((1u << binding_address_bits) - 1u);
  switch (address_dwords >> binding_address_bits) {
    case 0u:
      xe_shared_memory[0].data[binding_address] = value;
      break;
    case 1u:
      xe_shared_memory[1].data[binding_address] = value;
      break;
#if XE_SHARED_MEMORY_BINDING_COUNT_LOG2 >= 2
    case 2u:
      xe_shared_memory[2].data[binding_address] = value;
      break;
    case 3u:
      xe_shared_memory[3].data[binding_address] = value;
      break;
#endif
  }
#endif
}
)";

// Addressing shared with the scaled memory write-back compute shader; uses
// xe_native_resolve_dest_pitch and xe_native_resolve_scale.
constexpr char kNativeResolveAddressingFunctions[] = R"(
// Xenos 2D tiled texture addressing, in bytes.
int XeTiledOffset2D(int x, int y, uint pitch, uint bytes_per_block_log2) {
  int macro = ((x >> 5) + (y >> 5) * int(pitch >> 5)) << (bytes_per_block_log2 + 7u);
  int micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  int offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}
uint XeEndianSwap32(uint value, uint endian) {
  if (endian == 1u || endian == 2u) {
    value = ((value & 0x00FF00FFu) << 8u) | ((value >> 8u) & 0x00FF00FFu);
  }
  if (endian == 2u || endian == 3u) {
    value = (value << 16u) | (value >> 16u);
  }
  return value;
}
// Byte offset of host texel (x, y) of a scaled texture of resolved memory in
// the scaled resolve buffer, from the texture's base, in the layout this
// runtime's scaled resolve and texture load shaders use: host texels are in
// groups of 16 bytes of a row (4 texels at 32bpp, 2 at 64bpp), and the scale x
// scale host groups of each guest group are stored column-major at the guest
// group's tiled address times the scale area. (Newer Xenia uses taller groups;
// the layout must match the resolve_*_scaled shaders built into this runtime.)
// xe_native_resolve_scale: x | (y << 8) | (log2 x << 16) | (log2 y << 20), and
// bit 31 when both are powers of two (then shifts replace the divisions, which
// GPUs have no instruction for).
uint XeNativeResolveScaledOffset(ivec2 host_texel, uint bytes_per_block_log2) {
  uvec2 scale = uvec2(xe_native_resolve_scale & 0xFFu, (xe_native_resolve_scale >> 8u) & 0xFFu);
  uint group_texels_log2 = 4u - bytes_per_block_log2;
  uvec2 position = uvec2(host_texel);
  uvec2 host_group = uvec2(position.x >> group_texels_log2, position.y);
  uvec2 guest_group, host_group_in_guest_group;
  if ((xe_native_resolve_scale & 0x80000000u) != 0u) {
    uvec2 scale_log2 = uvec2((xe_native_resolve_scale >> 16u) & 0xFu,
                             (xe_native_resolve_scale >> 20u) & 0xFu);
    guest_group = host_group >> scale_log2;
    host_group_in_guest_group = host_group & (scale - 1u);
  } else {
    guest_group = host_group / scale;
    host_group_in_guest_group = host_group - guest_group * scale;
  }
  uvec2 guest_group_origin = uvec2(guest_group.x << group_texels_log2, guest_group.y);
  uint host_group_index = host_group_in_guest_group.x * scale.y + host_group_in_guest_group.y;
  uint guest_offset = uint(XeTiledOffset2D(int(guest_group_origin.x), int(guest_group_origin.y),
                                           xe_native_resolve_dest_pitch, bytes_per_block_log2));
  return guest_offset * (scale.x * scale.y) + (host_group_index << 4u) +
         ((position.x & ((1u << group_texels_log2) - 1u)) << bytes_per_block_log2);
}
)";

constexpr char kNativeResolveFragmentShaderFunctions[] = R"(
bool XeNativeResolveMemoryIs64bpp() {
  return (xe_native_resolve_flags & 64u) != 0u;
}
// Dword address in the bound memory of this fragment's texel.
uint XeNativeResolveMemoryAddress() {
  ivec2 texel = ivec2(gl_FragCoord.xy);
  uint bytes_per_block_log2 = XeNativeResolveMemoryIs64bpp() ? 3u : 2u;
  if ((xe_native_resolve_flags & 128u) != 0u) {
    return (XeNativeResolveScaledOffset(texel, bytes_per_block_log2) >> 2u) -
           xe_native_resolve_memory_base_dwords;
  }
  return xe_native_resolve_dest_base_dwords +
         (uint(XeTiledOffset2D(texel.x, texel.y, xe_native_resolve_dest_pitch,
                               bytes_per_block_log2)) >> 2u) -
         xe_native_resolve_memory_base_dwords;
}
// Stores the fragment's texel where the EDRAM resolve would write it. (Each
// fragment writing its own texel measured faster than one fragment per 16 bytes
// writing the whole run.)
void XeNativeResolveStoreMemory(uvec2 words) {
  bool is_64bpp = XeNativeResolveMemoryIs64bpp();
  uint endian = (xe_native_resolve_flags >> 4u) & 3u;
  uint address = XeNativeResolveMemoryAddress();
  XeSharedMemoryStore(address, XeEndianSwap32(words.x, endian));
  if (is_64bpp) {
    XeSharedMemoryStore(address + 1u, XeEndianSwap32(words.y, endian));
  }
}
bool XeNativeResolveWritesMemory() {
  return (xe_native_resolve_flags & 8u) != 0u;
}
ivec2 XeNativeResolveSourceCoord() {
  return ivec2(gl_FragCoord.xy) + xe_native_resolve_source_offset;
}
// Red / blue swap requested by the resolve, 8_8_8_8 and 2_10_10_10 only.
uint XeNativeResolveSwap(uint bits) {
  if ((xe_native_resolve_flags & 1u) == 0u) {
    return bits;
  }
  if (xe_native_resolve_packing == 0u) {
    return (bits & 0xFF00FF00u) | ((bits & 0xFFu) << 16u) | ((bits >> 16u) & 0xFFu);
  }
  return (bits & 0xC00FFC00u) | ((bits & 0x3FFu) << 20u) | ((bits >> 20u) & 0x3FFu);
}
)";

constexpr char kNativeResolveColorFloatBody[] = R"(
layout(set = 0, binding = 0) uniform texture2D xe_native_resolve_source;
uvec2 XeNativeResolvePack(ivec2 source_coord) {
  vec4 color = texelFetch(xe_native_resolve_source, source_coord, 0);
  uvec2 bits;
  if (xe_native_resolve_packing == 0u) {
    uvec4 c = uvec4(color * 255.0 + 0.5);
    bits = uvec2(c.r | (c.g << 8u) | (c.b << 16u) | (c.a << 24u), 0u);
  } else if (xe_native_resolve_packing == 1u) {
    uvec3 rgb = uvec3(color.rgb * 1023.0 + 0.5);
    uint a = uint(color.a * 3.0 + 0.5);
    bits = uvec2(rgb.r | (rgb.g << 10u) | (rgb.b << 20u) | (a << 30u), 0u);
  } else if (xe_native_resolve_packing == 2u) {
    bits = uvec2(packHalf2x16(color.rg), packHalf2x16(color.ba));
  } else {
    bits = floatBitsToUint(color.rg);
  }
  bits.x = XeNativeResolveSwap(bits.x);
  return bits;
}
void main() {
  ivec2 source_coord = XeNativeResolveSourceCoord();
  uvec2 bits = XeNativeResolvePack(source_coord);
  xe_native_resolve_output = uvec4(bits, 0u, 0u);
  if (XeNativeResolveWritesMemory()) {
    XeNativeResolveStoreMemory(bits);
  }
}
)";

// The color of the source, of the destination's unorm format, written through
// a view of that format: the attachment stores exactly the bits the integer
// view would get (the values are exact for the format, so conversion back to
// it is lossless). The memory gets the same bits as kColorFloat writes.
constexpr char kNativeResolveColorUnormBody[] = R"(
layout(set = 0, binding = 0) uniform texture2D xe_native_resolve_source;
uvec2 XeNativeResolvePack(vec4 color) {
  uvec2 bits;
  if (xe_native_resolve_packing == 0u) {
    uvec4 c = uvec4(color * 255.0 + 0.5);
    bits = uvec2(c.r | (c.g << 8u) | (c.b << 16u) | (c.a << 24u), 0u);
  } else {
    uvec3 rgb = uvec3(color.rgb * 1023.0 + 0.5);
    uint a = uint(color.a * 3.0 + 0.5);
    bits = uvec2(rgb.r | (rgb.g << 10u) | (rgb.b << 20u) | (a << 30u), 0u);
  }
  bits.x = XeNativeResolveSwap(bits.x);
  return bits;
}
void main() {
  ivec2 source_coord = XeNativeResolveSourceCoord();
  vec4 color = texelFetch(xe_native_resolve_source, source_coord, 0);
  // A preceding image copy may store the texture in the source's channel
  // order. Preserve that order for partial draws, independently of memory.
  bool texture_swap = ((xe_native_resolve_flags & 1u) != 0u) !=
                      ((xe_native_resolve_flags & 512u) != 0u);
  xe_native_resolve_output_float = texture_swap ? color.bgra : color;
  if (XeNativeResolveWritesMemory()) {
    XeNativeResolveStoreMemory(XeNativeResolvePack(color));
  }
}
)";

// Ownership transfer views of float render targets are integer to keep NaNs.
constexpr char kNativeResolveColorUintBody[] = R"(
layout(set = 0, binding = 0) uniform utexture2D xe_native_resolve_source;
uvec2 XeNativeResolvePack(ivec2 source_coord) {
  uvec4 u = texelFetch(xe_native_resolve_source, source_coord, 0);
  return xe_native_resolve_packing == 4u
             ? uvec2((u.x & 0xFFFFu) | (u.y << 16u), (u.z & 0xFFFFu) | (u.w << 16u))
             : u.xy;
}
void main() {
  ivec2 source_coord = XeNativeResolveSourceCoord();
  uvec2 bits = XeNativeResolvePack(source_coord);
  xe_native_resolve_output = uvec4(bits, 0u, 0u);
  if (XeNativeResolveWritesMemory()) {
    XeNativeResolveStoreMemory(bits);
  }
}
)";

// The same conversions as dumping host depth to the EDRAM (float24 from host
// depth remapped from the guest [0, 2) to [0, 1), or unorm24), followed by
// loading the resolved k_24_8_FLOAT or k_24_8 texture into R32_SFLOAT.
constexpr char kNativeResolveDepthBody[] = R"(
layout(set = 0, binding = 0) uniform texture2D xe_native_resolve_source;
layout(set = 0, binding = 1) uniform utexture2D xe_native_resolve_stencil;
uint XeHostDepthTo20e4(uint f32, bool round_to_nearest_even) {
  uint denormal = ((f32 & 0x7FFFFFu) | 0x800000u) >> min(112u - (f32 >> 23u), 24u);
  uint biased = f32 < 0x38000000u ? denormal : f32 - (111u << 23u);
  if (round_to_nearest_even) {
    biased += 3u + ((biased >> 3u) & 1u);
  }
  return (biased >> 3u) & 0xFFFFFFu;
}
float XeFloat20e4To32(uint f24) {
  if (f24 == 0u) {
    return 0.0;
  }
  uint mantissa = f24 & 0xFFFFFu;
  uint exponent = f24 >> 20u;
  if (exponent == 0u) {
    uint msb = uint(findMSB(mantissa));
    exponent = msb - 19u;
    mantissa = (mantissa << (20u - msb)) & 0xFFFFFu;
  }
  return uintBitsToFloat(((exponent + 112u) << 23u) | (mantissa << 3u));
}
uint XeNativeResolveDepth24(ivec2 source_coord) {
  float depth = texelFetch(xe_native_resolve_source, source_coord, 0).r;
  if ((xe_native_resolve_flags & 2u) != 0u) {
    return XeHostDepthTo20e4(floatBitsToUint(depth), (xe_native_resolve_flags & 4u) != 0u);
  }
  return uint(roundEven(depth * 16777215.0));
}
uvec2 XeNativeResolveMemoryWords(ivec2 source_coord, uint depth24) {
  uint stencil = texelFetch(xe_native_resolve_stencil, source_coord, 0).r & 0xFFu;
  return uvec2((depth24 << 8u) | stencil, 0u);
}
#if XE_NATIVE_RESOLVE_STENCIL_CAPTURE
// Stores the stencil of this 2x2 quad of host pixels as one dword: (x, y),
// (x + 1, y), (x, y + 1), (x + 1, y + 1) from the low byte, which is the order
// of the invocations of a fragment shader quad. The first invocation is at even
// coordinates on the hardware (not required by Vulkan, so checked here, and
// verified with native_resolve_debug_verify_stencil_capture).
void XeNativeResolveCaptureStencil(ivec2 source_coord) {
  uint stencil = texelFetch(xe_native_resolve_stencil, source_coord, 0).r & 0xFFu;
  uint quad_stencil = subgroupQuadBroadcast(stencil, 0u) |
                      (subgroupQuadBroadcast(stencil, 1u) << 8u) |
                      (subgroupQuadBroadcast(stencil, 2u) << 16u) |
                      (subgroupQuadBroadcast(stencil, 3u) << 24u);
  uvec2 position = uvec2(gl_FragCoord.xy);
  if ((gl_SubgroupInvocationID & 3u) == 0u && ((position.x | position.y) & 1u) == 0u) {
    uvec2 origin = uvec2(xe_native_resolve_stencil_capture_origin & 0xFFFFu,
                         xe_native_resolve_stencil_capture_origin >> 16u);
    uvec2 quad = (position - origin) >> 1u;
    XeSharedMemoryStore(quad.y * xe_native_resolve_stencil_capture_pitch_quads + quad.x,
                        quad_stencil);
  }
}
#endif
void main() {
  ivec2 source_coord = XeNativeResolveSourceCoord();
#if XE_NATIVE_RESOLVE_STENCIL_CAPTURE
  if ((xe_native_resolve_flags & 256u) != 0u) {
    XeNativeResolveCaptureStencil(source_coord);
  }
#endif
  uint depth24 = XeNativeResolveDepth24(source_coord);
  float result;
  if ((xe_native_resolve_flags & 2u) != 0u) {
    result = XeFloat20e4To32(depth24);
  } else {
    result = float(depth24 + (depth24 >> 23u)) * 5.96046448e-08;
  }
#if XE_NATIVE_RESOLVE_FLOAT_OUTPUT
  // Exact: the results are zero or normal floats.
  xe_native_resolve_output_float = vec4(result, 0.0, 0.0, 0.0);
#else
  xe_native_resolve_output = uvec4(floatBitsToUint(result), 0u, 0u, 0u);
#endif
  if (XeNativeResolveWritesMemory()) {
    XeNativeResolveStoreMemory(XeNativeResolveMemoryWords(source_coord, depth24));
  }
}
)";

// Writes a texture a native resolve wrote (its raw bits view) to the scaled
// resolve memory, where the resolve would have written it
// (native_resolve_scaled_lazy_memory). One invocation per host texel of the
// resolved rectangle; the memory is bound from the destination's base.
constexpr char kScaledMemoryWritebackShaderHeader[] = R"(#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(push_constant) uniform XeScaledMemoryWritebackConstants {
  uvec2 xe_writeback_origin;
  uvec2 xe_writeback_size;
  uint xe_native_resolve_flags;
  uint xe_native_resolve_dest_pitch;
  uint xe_native_resolve_scale;
  // Depth write-back: the native resolve flags (bit 1 = float24 depth).
  uint xe_writeback_depth_flags;
};
#if XE_WRITEBACK_FLOAT_SOURCE
layout(set = 0, binding = 0) uniform texture2D xe_writeback_source;
#else
layout(set = 0, binding = 0) uniform utexture2D xe_writeback_source;
#endif
layout(set = 1, binding = 0) buffer XeWritebackMemory {
  uint data[];
} xe_writeback_memory;
)";
constexpr char kScaledMemoryWritebackShaderMain[] = R"(
void main() {
  uvec2 position = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(position, xe_writeback_size))) {
    return;
  }
  ivec2 texel = ivec2(xe_writeback_origin + position);
  uvec4 bits = texelFetch(xe_writeback_source, texel, 0);
  bool is_64bpp = (xe_native_resolve_flags & 64u) != 0u;
  uint endian = (xe_native_resolve_flags >> 4u) & 3u;
  uint address = XeNativeResolveScaledOffset(texel, is_64bpp ? 3u : 2u) >> 2u;
  xe_writeback_memory.data[address] = XeEndianSwap32(bits.x, endian);
  if (is_64bpp) {
    xe_writeback_memory.data[address + 1u] = XeEndianSwap32(bits.y, endian);
  }
}
)";

// A unorm view of an 8_8_8_8 (packing 0 in xe_writeback_depth_flags) or
// 2_10_10_10 (packing 1) texture, packed to its bits like the native resolve
// does (the texture already has the red / blue swap applied).
constexpr char kScaledMemoryWritebackUnormShaderMain[] = R"(
void main() {
  uvec2 position = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(position, xe_writeback_size))) {
    return;
  }
  ivec2 texel = ivec2(xe_writeback_origin + position);
  vec4 color = texelFetch(xe_writeback_source, texel, 0);
  if ((xe_writeback_depth_flags & 4u) != 0u) {
    color = color.bgra;
  }
  uint bits;
  if ((xe_writeback_depth_flags & 1u) == 0u) {
    uvec4 c = uvec4(color * 255.0 + 0.5);
    bits = c.r | (c.g << 8u) | (c.b << 16u) | (c.a << 24u);
  } else {
    uvec3 rgb = uvec3(color.rgb * 1023.0 + 0.5);
    uint a = uint(color.a * 3.0 + 0.5);
    bits = rgb.r | (rgb.g << 10u) | (rgb.b << 20u) | (a << 30u);
  }
  uint endian = (xe_native_resolve_flags >> 4u) & 3u;
  uint address = XeNativeResolveScaledOffset(texel, 2u) >> 2u;
  xe_writeback_memory.data[address] = XeEndianSwap32(bits, endian);
}
)";

// Depth: the texture holds the depth converted as the depth native resolve
// converts it (exactly invertible), and the stencil the memory also holds is
// in the capture (set 2), one byte per texel of the rectangle, row by row.
constexpr char kScaledMemoryWritebackDepthShaderMain[] = R"(
layout(set = 2, binding = 0) buffer XeWritebackStencil {
  uint data[];
} xe_writeback_stencil;
// With native_resolve_debug_verify_stencil_capture, the copied capture.
layout(set = 3, binding = 0) buffer XeWritebackStencilVerify {
  uint data[];
} xe_writeback_stencil_verify;
uint XeWritebackStencilByte(uint stencil_word, uint byte_index) {
  return (stencil_word >> (byte_index << 3u)) & 0xFFu;
}
// Inverse of XeFloat20e4To32 of the native resolve (exact for its results).
uint XeFloat32To20e4(uint f32) {
  if (f32 == 0u) {
    return 0u;
  }
  uint exponent = f32 >> 23u;
  uint mantissa = (f32 >> 3u) & 0xFFFFFu;
  if (exponent >= 113u) {
    return ((exponent - 112u) << 20u) | mantissa;
  }
  return (mantissa | 0x100000u) >> (113u - exponent);
}
void main() {
  uvec2 position = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(position, xe_writeback_size))) {
    return;
  }
  ivec2 texel = ivec2(xe_writeback_origin + position);
#if XE_WRITEBACK_FLOAT_SOURCE
  uint depth_bits = floatBitsToUint(texelFetch(xe_writeback_source, texel, 0).x);
#else
  uint depth_bits = texelFetch(xe_writeback_source, texel, 0).x;
#endif
  uint depth24;
  if ((xe_writeback_depth_flags & 2u) != 0u) {
    depth24 = XeFloat32To20e4(depth_bits);
  } else {
    // Inverse of float(depth24 + (depth24 >> 23)) * 2^-24.
    uint value = uint(uintBitsToFloat(depth_bits) * 16777216.0);
    depth24 = value >= 0x800000u ? value - 1u : value;
  }
  uint stencil_index = position.y * xe_writeback_size.x + position.x;
  uint stencil;
  if ((xe_writeback_depth_flags & 256u) != 0u) {
    // A dword per 2x2 quad, captured by the native resolve.
    stencil = XeWritebackStencilByte(
        xe_writeback_stencil.data[(position.y >> 1u) * (xe_writeback_size.x >> 1u) +
                                  (position.x >> 1u)],
        ((position.y & 1u) << 1u) | (position.x & 1u));
  } else {
    // Copied, a byte per texel.
    stencil = XeWritebackStencilByte(xe_writeback_stencil.data[stencil_index >> 2u],
                                     stencil_index & 3u);
  }
  if ((xe_writeback_depth_flags & 512u) != 0u &&
      XeWritebackStencilByte(xe_writeback_stencil_verify.data[stencil_index >> 2u],
                             stencil_index & 3u) != stencil) {
    // Visible in textures reloaded from the memory.
    depth24 = 0u;
  }
  uint endian = (xe_native_resolve_flags >> 4u) & 3u;
  uint address = XeNativeResolveScaledOffset(texel, 2u) >> 2u;
  xe_writeback_memory.data[address] = XeEndianSwap32((depth24 << 8u) | stencil, endian);
}
)";

struct ScaledMemoryWritebackConstants {
  uint32_t origin_x;
  uint32_t origin_y;
  uint32_t size_x;
  uint32_t size_y;
  uint32_t flags;
  uint32_t dest_pitch_texels;
  uint32_t resolution_scale;
  uint32_t depth_flags;
};

}  // namespace

bool VulkanRenderTargetCache::InitializeNativeResolve(uint32_t shared_memory_binding_count) {
  native_resolve_shared_memory_binding_count_ = shared_memory_binding_count;
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  if (!REXCVAR_GET(vulkan_dynamic_rendering) || !vulkan_device->properties().dynamicRendering) {
    REXGPU_INFO("VulkanRenderTargetCache: native resolves need dynamic rendering, disabled");
    return false;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // The depth native resolve captures the stencil for lazy scaled memory with
  // fragment shader quad subgroup operations (Vulkan 1.1).
  native_resolve_quad_stencil_capture_ = false;
  if (vulkan_device->properties().apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0) &&
      vulkan_device->vulkan_instance()->functions().vkGetPhysicalDeviceProperties2) {
    VkPhysicalDeviceSubgroupProperties subgroup_properties = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 properties_2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties_2.pNext = &subgroup_properties;
    vulkan_device->vulkan_instance()->functions().vkGetPhysicalDeviceProperties2(
        vulkan_device->physical_device(), &properties_2);
    native_resolve_quad_stencil_capture_ =
        (subgroup_properties.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) &&
        (subgroup_properties.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) &&
        (subgroup_properties.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT);
  }

  auto compile = [&](VkShaderStageFlagBits stage, const std::string& source,
                     bool spirv_1_3 = false) -> VkShaderModule {
    std::vector<uint32_t> spirv;
    std::string error;
    if (!command_processor_.CompileGlslToSpirv(stage, source, spirv, error, spirv_1_3)) {
      REXGPU_ERROR("VulkanRenderTargetCache: failed to compile a native resolve shader: {}",
                   error);
      return VK_NULL_HANDLE;
    }
    return ui::vulkan::util::CreateShaderModule(vulkan_device, spirv.data(),
                                                sizeof(uint32_t) * spirv.size());
  };
  native_resolve_vertex_shader_ =
      compile(VK_SHADER_STAGE_VERTEX_BIT, kNativeResolveVertexShaderSource);
  const char* const fragment_bodies[size_t(NativeResolveShader::kCount)] = {
      kNativeResolveColorFloatBody,
      kNativeResolveColorUintBody,
      kNativeResolveDepthBody,
      kNativeResolveColorUnormBody,
      kNativeResolveDepthBody,
  };
  uint32_t shared_memory_binding_count_log2 = 0;
  rex::bit_scan_forward(std::max(shared_memory_binding_count, uint32_t(1)),
                        &shared_memory_binding_count_log2);
  std::string fragment_prefix =
      fmt::format("#version 450\n#define XE_SHARED_MEMORY_BINDING_COUNT_LOG2 {}\n",
                  shared_memory_binding_count_log2);
  bool shaders_created = native_resolve_vertex_shader_ != VK_NULL_HANDLE;
  for (size_t i = 0; i < size_t(NativeResolveShader::kCount); ++i) {
    std::string fragment_source = std::string(kNativeResolveFragmentShaderHeader) +
                                  kNativeResolveAddressingFunctions +
                                  kNativeResolveFragmentShaderFunctions + fragment_bodies[i];
    const char* float_output_define = (i == size_t(NativeResolveShader::kColorUnorm) ||
                                       i == size_t(NativeResolveShader::kDepthFloat))
                                          ? "#define XE_NATIVE_RESOLVE_FLOAT_OUTPUT 1\n"
                                          : "";
    if (IsNativeResolveDepthShader(NativeResolveShader(i)) &&
        native_resolve_quad_stencil_capture_) {
      native_resolve_fragment_shaders_[i] =
          compile(VK_SHADER_STAGE_FRAGMENT_BIT,
                  fragment_prefix +
                      "#extension GL_KHR_shader_subgroup_basic : require\n"
                      "#extension GL_KHR_shader_subgroup_quad : require\n"
                      "#define XE_NATIVE_RESOLVE_STENCIL_CAPTURE 1\n" +
                      float_output_define + fragment_source,
                  true);
      if (native_resolve_fragment_shaders_[i] == VK_NULL_HANDLE) {
        REXGPU_WARN("VulkanRenderTargetCache: depth native resolves capture the stencil by "
                    "copying instead");
        native_resolve_quad_stencil_capture_ = false;
      }
    }
    if (native_resolve_fragment_shaders_[i] == VK_NULL_HANDLE) {
      native_resolve_fragment_shaders_[i] =
          compile(VK_SHADER_STAGE_FRAGMENT_BIT, fragment_prefix + float_output_define +
                                                    fragment_source);
    }
    shaders_created &= native_resolve_fragment_shaders_[i] != VK_NULL_HANDLE;
  }
  if (!shaders_created) {
    ShutdownNativeResolve();
    return false;
  }

  REXGPU_INFO("VulkanRenderTargetCache: native resolves enabled");
  return true;
}

bool VulkanRenderTargetCache::EnsureNativeResolvePipelineLayouts() {
  if (native_resolve_pipeline_layout_color_ != VK_NULL_HANDLE) {
    return true;
  }
  VkDescriptorSetLayout shared_memory_layout =
      command_processor_.descriptor_set_layout_shared_memory_and_edram();
  if (shared_memory_layout == VK_NULL_HANDLE) {
    return false;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  auto create_pipeline_layout = [&](VkDescriptorSetLayout source_layout,
                                    VkPipelineLayout& pipeline_layout_out) {
    VkDescriptorSetLayout set_layouts[] = {source_layout, shared_memory_layout};
    VkPushConstantRange push_constant_range;
    push_constant_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push_constant_range.offset = 0;
    push_constant_range.size = sizeof(NativeResolveConstants);
    VkPipelineLayoutCreateInfo pipeline_layout_create_info;
    pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.pNext = nullptr;
    pipeline_layout_create_info.flags = 0;
    pipeline_layout_create_info.setLayoutCount = uint32_t(rex::countof(set_layouts));
    pipeline_layout_create_info.pSetLayouts = set_layouts;
    pipeline_layout_create_info.pushConstantRangeCount = 1;
    pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr,
                                   &pipeline_layout_out) != VK_SUCCESS) {
      pipeline_layout_out = VK_NULL_HANDLE;
      return false;
    }
    return true;
  };
  // Depth sources are bound with the depth and stencil transfer descriptor set.
  if (!create_pipeline_layout(descriptor_set_layout_sampled_image_,
                              native_resolve_pipeline_layout_color_) ||
      !create_pipeline_layout(descriptor_set_layout_sampled_image_x2_,
                              native_resolve_pipeline_layout_depth_)) {
    REXGPU_ERROR("VulkanRenderTargetCache: failed to create the native resolve pipeline layouts");
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                           native_resolve_pipeline_layout_depth_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                           native_resolve_pipeline_layout_color_);
    return false;
  }
  return true;
}

void VulkanRenderTargetCache::ShutdownNativeResolve() {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  pending_scaled_resolve_memory_.clear();
  scaled_memory_writeback_descriptors_.clear();
  for (ScaledStencilCapture& capture : scaled_stencil_captures_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, capture.buffer);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, capture.memory);
  }
  scaled_stencil_captures_.clear();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         scaled_memory_writeback_depth_float_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         scaled_memory_writeback_depth_float_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         scaled_memory_writeback_depth_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         scaled_memory_writeback_depth_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         scaled_memory_writeback_depth_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         scaled_memory_writeback_unorm_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         scaled_memory_writeback_unorm_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         scaled_memory_writeback_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         scaled_memory_writeback_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         scaled_memory_writeback_shader_);
  for (const auto& pipeline_pair : native_resolve_pipelines_) {
    if (pipeline_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pipeline_pair.second, nullptr);
    }
  }
  native_resolve_pipelines_.clear();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         native_resolve_pipeline_layout_depth_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         native_resolve_pipeline_layout_color_);
  for (VkShaderModule& shader : native_resolve_fragment_shaders_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, shader);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         native_resolve_vertex_shader_);
  native_resolve_enabled_ = false;
}

VkPipeline VulkanRenderTargetCache::GetNativeResolvePipeline(NativeResolveShader shader,
                                                             VkFormat dest_format) {
  uint64_t key = (uint64_t(dest_format) << 8) | uint64_t(shader);
  auto it = native_resolve_pipelines_.find(key);
  if (it != native_resolve_pipelines_.end()) {
    return it->second;
  }
  if (!EnsureNativeResolvePipelineLayouts()) {
    return VK_NULL_HANDLE;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkPipelineShaderStageCreateInfo shader_stages[2] = {};
  shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  shader_stages[0].module = native_resolve_vertex_shader_;
  shader_stages[0].pName = "main";
  shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  shader_stages[1].module = native_resolve_fragment_shaders_[size_t(shader)];
  shader_stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertex_input_state = {};
  vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {};
  input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport_state = {};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterization_state = {};
  rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization_state.cullMode = VK_CULL_MODE_NONE;
  rasterization_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterization_state.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample_state = {};
  multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {};
  depth_stencil_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

  VkPipelineColorBlendAttachmentState color_blend_attachment = {};
  color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blend_state.attachmentCount = 1;
  color_blend_state.pAttachments = &color_blend_attachment;

  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic_state = {};
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.dynamicStateCount = uint32_t(rex::countof(dynamic_states));
  dynamic_state.pDynamicStates = dynamic_states;

  VkPipelineRenderingCreateInfo pipeline_rendering_create_info = {};
  pipeline_rendering_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  pipeline_rendering_create_info.colorAttachmentCount = 1;
  pipeline_rendering_create_info.pColorAttachmentFormats = &dest_format;

  VkGraphicsPipelineCreateInfo pipeline_create_info = {};
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext = &pipeline_rendering_create_info;
  pipeline_create_info.stageCount = uint32_t(rex::countof(shader_stages));
  pipeline_create_info.pStages = shader_stages;
  pipeline_create_info.pVertexInputState = &vertex_input_state;
  pipeline_create_info.pInputAssemblyState = &input_assembly_state;
  pipeline_create_info.pViewportState = &viewport_state;
  pipeline_create_info.pRasterizationState = &rasterization_state;
  pipeline_create_info.pMultisampleState = &multisample_state;
  pipeline_create_info.pDepthStencilState = &depth_stencil_state;
  pipeline_create_info.pColorBlendState = &color_blend_state;
  pipeline_create_info.pDynamicState = &dynamic_state;
  pipeline_create_info.layout = IsNativeResolveDepthShader(shader)
                                    ? native_resolve_pipeline_layout_depth_
                                    : native_resolve_pipeline_layout_color_;
  pipeline_create_info.renderPass = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;
  VkPipeline pipeline;
  if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr,
                                    &pipeline) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: failed to create the native resolve pipeline for shader {}, "
        "format {}",
        uint32_t(shader), uint32_t(dest_format));
    pipeline = VK_NULL_HANDLE;
  }
  native_resolve_pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool VulkanRenderTargetCache::EnsureScaledMemoryWritebackPipeline() {
  if (scaled_memory_writeback_pipeline_ != VK_NULL_HANDLE) {
    return true;
  }
  if (scaled_memory_writeback_pipeline_failed_) {
    return false;
  }
  scaled_memory_writeback_pipeline_failed_ = true;
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  std::vector<uint32_t> spirv;
  std::string error;
  if (!command_processor_.CompileGlslToSpirv(
          VK_SHADER_STAGE_COMPUTE_BIT,
          std::string(kScaledMemoryWritebackShaderHeader) + kNativeResolveAddressingFunctions +
              kScaledMemoryWritebackShaderMain,
          spirv, error)) {
    REXGPU_ERROR("VulkanRenderTargetCache: failed to compile the scaled memory write-back "
                 "shader: {}",
                 error);
    return false;
  }
  scaled_memory_writeback_shader_ = ui::vulkan::util::CreateShaderModule(
      vulkan_device, spirv.data(), sizeof(uint32_t) * spirv.size());
  if (scaled_memory_writeback_shader_ == VK_NULL_HANDLE) {
    return false;
  }
  VkDescriptorSetLayout set_layouts[] = {
      descriptor_set_layout_sampled_image_,
      command_processor_.GetSingleTransientDescriptorLayout(
          VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferCompute),
  };
  VkPushConstantRange push_constant_range;
  push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_constant_range.offset = 0;
  push_constant_range.size = sizeof(ScaledMemoryWritebackConstants);
  VkPipelineLayoutCreateInfo pipeline_layout_create_info;
  pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_create_info.pNext = nullptr;
  pipeline_layout_create_info.flags = 0;
  pipeline_layout_create_info.setLayoutCount = uint32_t(rex::countof(set_layouts));
  pipeline_layout_create_info.pSetLayouts = set_layouts;
  pipeline_layout_create_info.pushConstantRangeCount = 1;
  pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr,
                                 &scaled_memory_writeback_pipeline_layout_) != VK_SUCCESS) {
    scaled_memory_writeback_pipeline_layout_ = VK_NULL_HANDLE;
    return false;
  }
  scaled_memory_writeback_pipeline_ = ui::vulkan::util::CreateComputePipeline(
      vulkan_device, scaled_memory_writeback_pipeline_layout_, scaled_memory_writeback_shader_);
  if (scaled_memory_writeback_pipeline_ == VK_NULL_HANDLE) {
    return false;
  }
  scaled_memory_writeback_pipeline_failed_ = false;
  return true;
}

bool VulkanRenderTargetCache::EnsureScaledMemoryWritebackUnormPipeline() {
  if (scaled_memory_writeback_unorm_pipeline_ != VK_NULL_HANDLE) {
    return true;
  }
  if (scaled_memory_writeback_unorm_pipeline_failed_ || !EnsureScaledMemoryWritebackPipeline()) {
    return false;
  }
  scaled_memory_writeback_unorm_pipeline_failed_ = true;
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  std::vector<uint32_t> spirv;
  std::string error;
  std::string header(kScaledMemoryWritebackShaderHeader);
  header.insert(header.find('\n') + 1, "#define XE_WRITEBACK_FLOAT_SOURCE 1\n");
  if (!command_processor_.CompileGlslToSpirv(
          VK_SHADER_STAGE_COMPUTE_BIT,
          header + kNativeResolveAddressingFunctions + kScaledMemoryWritebackUnormShaderMain, spirv,
          error)) {
    REXGPU_ERROR("VulkanRenderTargetCache: failed to compile the unorm scaled memory write-back "
                 "shader: {}",
                 error);
    return false;
  }
  scaled_memory_writeback_unorm_shader_ = ui::vulkan::util::CreateShaderModule(
      vulkan_device, spirv.data(), sizeof(uint32_t) * spirv.size());
  if (scaled_memory_writeback_unorm_shader_ == VK_NULL_HANDLE) {
    return false;
  }
  scaled_memory_writeback_unorm_pipeline_ = ui::vulkan::util::CreateComputePipeline(
      vulkan_device, scaled_memory_writeback_pipeline_layout_,
      scaled_memory_writeback_unorm_shader_);
  if (scaled_memory_writeback_unorm_pipeline_ == VK_NULL_HANDLE) {
    return false;
  }
  scaled_memory_writeback_unorm_pipeline_failed_ = false;
  return true;
}

bool VulkanRenderTargetCache::EnsureScaledMemoryWritebackDepthFloatPipeline() {
  if (scaled_memory_writeback_depth_float_pipeline_ != VK_NULL_HANDLE) {
    return true;
  }
  if (scaled_memory_writeback_depth_float_pipeline_failed_ ||
      !EnsureScaledMemoryWritebackDepthPipeline()) {
    return false;
  }
  scaled_memory_writeback_depth_float_pipeline_failed_ = true;
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  std::string header(kScaledMemoryWritebackShaderHeader);
  header.insert(header.find('\n') + 1, "#define XE_WRITEBACK_FLOAT_SOURCE 1\n");
  std::vector<uint32_t> spirv;
  std::string error;
  if (!command_processor_.CompileGlslToSpirv(
          VK_SHADER_STAGE_COMPUTE_BIT,
          header + kNativeResolveAddressingFunctions + kScaledMemoryWritebackDepthShaderMain, spirv,
          error)) {
    REXGPU_ERROR("VulkanRenderTargetCache: failed to compile the float scaled memory depth "
                 "write-back shader: {}",
                 error);
    return false;
  }
  scaled_memory_writeback_depth_float_shader_ = ui::vulkan::util::CreateShaderModule(
      vulkan_device, spirv.data(), sizeof(uint32_t) * spirv.size());
  if (scaled_memory_writeback_depth_float_shader_ == VK_NULL_HANDLE) {
    return false;
  }
  scaled_memory_writeback_depth_float_pipeline_ = ui::vulkan::util::CreateComputePipeline(
      vulkan_device, scaled_memory_writeback_depth_pipeline_layout_,
      scaled_memory_writeback_depth_float_shader_);
  if (scaled_memory_writeback_depth_float_pipeline_ == VK_NULL_HANDLE) {
    return false;
  }
  scaled_memory_writeback_depth_float_pipeline_failed_ = false;
  return true;
}

bool VulkanRenderTargetCache::EnsureScaledMemoryWritebackDepthPipeline() {
  if (scaled_memory_writeback_depth_pipeline_ != VK_NULL_HANDLE) {
    return true;
  }
  if (scaled_memory_writeback_depth_pipeline_failed_) {
    return false;
  }
  scaled_memory_writeback_depth_pipeline_failed_ = true;
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  std::vector<uint32_t> spirv;
  std::string error;
  if (!command_processor_.CompileGlslToSpirv(
          VK_SHADER_STAGE_COMPUTE_BIT,
          std::string(kScaledMemoryWritebackShaderHeader) + kNativeResolveAddressingFunctions +
              kScaledMemoryWritebackDepthShaderMain,
          spirv, error)) {
    REXGPU_ERROR("VulkanRenderTargetCache: failed to compile the scaled memory depth write-back "
                 "shader: {}",
                 error);
    return false;
  }
  scaled_memory_writeback_depth_shader_ = ui::vulkan::util::CreateShaderModule(
      vulkan_device, spirv.data(), sizeof(uint32_t) * spirv.size());
  if (scaled_memory_writeback_depth_shader_ == VK_NULL_HANDLE) {
    return false;
  }
  VkDescriptorSetLayout storage_buffer_layout =
      command_processor_.GetSingleTransientDescriptorLayout(
          VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferCompute);
  VkDescriptorSetLayout set_layouts[] = {
      descriptor_set_layout_sampled_image_,
      storage_buffer_layout,
      storage_buffer_layout,
      storage_buffer_layout,
  };
  VkPushConstantRange push_constant_range;
  push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_constant_range.offset = 0;
  push_constant_range.size = sizeof(ScaledMemoryWritebackConstants);
  VkPipelineLayoutCreateInfo pipeline_layout_create_info;
  pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_create_info.pNext = nullptr;
  pipeline_layout_create_info.flags = 0;
  pipeline_layout_create_info.setLayoutCount = uint32_t(rex::countof(set_layouts));
  pipeline_layout_create_info.pSetLayouts = set_layouts;
  pipeline_layout_create_info.pushConstantRangeCount = 1;
  pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr,
                                 &scaled_memory_writeback_depth_pipeline_layout_) != VK_SUCCESS) {
    scaled_memory_writeback_depth_pipeline_layout_ = VK_NULL_HANDLE;
    return false;
  }
  scaled_memory_writeback_depth_pipeline_ = ui::vulkan::util::CreateComputePipeline(
      vulkan_device, scaled_memory_writeback_depth_pipeline_layout_,
      scaled_memory_writeback_depth_shader_);
  if (scaled_memory_writeback_depth_pipeline_ == VK_NULL_HANDLE) {
    return false;
  }
  scaled_memory_writeback_depth_pipeline_failed_ = false;
  return true;
}

uint32_t VulkanRenderTargetCache::AcquireScaledStencilCapture(VkDeviceSize size) {
  size = rex::align(size, VkDeviceSize(4));
  uint32_t best = UINT32_MAX;
  for (uint32_t i = 0; i < uint32_t(scaled_stencil_captures_.size()); ++i) {
    const ScaledStencilCapture& capture = scaled_stencil_captures_[i];
    if (capture.in_use || capture.size < size) {
      continue;
    }
    if (best == UINT32_MAX || capture.size < scaled_stencil_captures_[best].size) {
      best = i;
    }
  }
  if (best == UINT32_MAX) {
    // Pending resolves are replaced by the next frame's resolves of the same
    // memory, so only a few are held at once; past this, resolve eagerly.
    constexpr size_t kMaxCaptures = 16;
    if (scaled_stencil_captures_.size() >= kMaxCaptures) {
      return UINT32_MAX;
    }
    ScaledStencilCapture capture;
    // Rounded up so resolves of similar sizes can share captures.
    capture.size = rex::align(size, VkDeviceSize(1) << 20);
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            command_processor_.GetVulkanDevice(), capture.size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal, capture.buffer, capture.memory)) {
      REXGPU_ERROR("VulkanRenderTargetCache: failed to create a {} byte stencil capture buffer",
                   capture.size);
      return UINT32_MAX;
    }
    best = uint32_t(scaled_stencil_captures_.size());
    scaled_stencil_captures_.push_back(capture);
  }
  scaled_stencil_captures_[best].in_use = true;
  return best;
}

void VulkanRenderTargetCache::ReleaseScaledStencilCapture(uint32_t index) {
  if (index < scaled_stencil_captures_.size()) {
    scaled_stencil_captures_[index].in_use = false;
  }
}

void VulkanRenderTargetCache::CaptureScaledResolveStencil(VulkanRenderTarget& source, uint32_t x0,
                                                          uint32_t y0, uint32_t x1, uint32_t y1,
                                                          uint32_t capture_index) {
  const ScaledStencilCapture& capture = scaled_stencil_captures_[capture_index];
  uint32_t scale_x = draw_resolution_scale_x(), scale_y = draw_resolution_scale_y();
  command_processor_.EndRenderPass();
  // The capture may still be read by the write-back of an earlier resolve, or
  // be being written by an earlier capture.
  command_processor_.PushBufferMemoryBarrier(
      capture.buffer, 0, VK_WHOLE_SIZE,
      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  command_processor_.PushImageMemoryBarrier(
      source.image(),
      ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                   VK_IMAGE_ASPECT_STENCIL_BIT),
      source.current_stage_mask(), VK_PIPELINE_STAGE_TRANSFER_BIT, source.current_access_mask(),
      VK_ACCESS_TRANSFER_READ_BIT, source.current_layout(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  source.SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  command_processor_.SubmitBarriers(true);
  // The resolved texel (x, y) of the target is the source's host pixel (x, y).
  VkBufferImageCopy region = {};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageOffset.x = int32_t(x0 * scale_x);
  region.imageOffset.y = int32_t(y0 * scale_y);
  region.imageExtent.width = (x1 - x0) * scale_x;
  region.imageExtent.height = (y1 - y0) * scale_y;
  region.imageExtent.depth = 1;
  command_processor_.deferred_command_buffer().CmdVkCopyImageToBuffer(
      source.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture.buffer, 1, &region);
  command_processor_.PushBufferMemoryBarrier(
      capture.buffer, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
}

void VulkanRenderTargetCache::FillScaledStencilCapture(uint32_t capture_index, uint32_t value,
                                                       VkDeviceSize size) {
  const ScaledStencilCapture& capture = scaled_stencil_captures_[capture_index];
  command_processor_.EndRenderPass();
  // The capture may still be read by the write-back of an earlier resolve, or
  // be being written by an earlier capture.
  command_processor_.PushBufferMemoryBarrier(
      capture.buffer, 0, VK_WHOLE_SIZE,
      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  command_processor_.SubmitBarriers(true);
  command_processor_.deferred_command_buffer().CmdVkFillBuffer(
      capture.buffer, 0, std::min(rex::align(size, VkDeviceSize(4)), capture.size),
      (value & 0xFF) * 0x01010101u);
  command_processor_.PushBufferMemoryBarrier(
      capture.buffer, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
}

void VulkanRenderTargetCache::DropPendingScaledResolveMemory(
    const PendingScaledResolveMemory& pending) {
  pending_scaled_resolve_texture_cache_->AddScaledMemoryPending(pending.texture, -1);
  if (pending.stencil_capture != UINT32_MAX) {
    ReleaseScaledStencilCapture(pending.stencil_capture);
  }
  if (pending.verify_stencil_capture != UINT32_MAX) {
    ReleaseScaledStencilCapture(pending.verify_stencil_capture);
  }
}

void VulkanRenderTargetCache::WriteBackPendingScaledResolveMemory(
    const PendingScaledResolveMemory& pending_resolve) {
  // The texture must hold the data first.
  FlushPendingCopyFreeResolve();
  VulkanTextureCache& texture_cache = *pending_scaled_resolve_texture_cache_;
  bool written = false;
  // Holds the capture filled for a uniform stencil, released with the rest.
  PendingScaledResolveMemory pending = pending_resolve;
  bool stencil_buffers_valid = true;
  if (pending.is_depth) {
    VkDeviceSize stencil_bytes =
        (VkDeviceSize(pending.x1 - pending.x0) * draw_resolution_scale_x() *
             (pending.y1 - pending.y0) * draw_resolution_scale_y() +
         3) &
        ~VkDeviceSize(3);
    if (pending.stencil_uniform && pending.stencil_capture == UINT32_MAX) {
      pending.stencil_capture = AcquireScaledStencilCapture(stencil_bytes);
      pending.stencil_capture_quads = false;
      if (pending.stencil_capture != UINT32_MAX) {
        FillScaledStencilCapture(pending.stencil_capture, pending.stencil_uniform_value,
                                 stencil_bytes);
      }
    }
    // The stencil buffers the write-back reads must exist and cover the
    // rectangle (a byte per host texel) - out of bounds reads fault the GPU.
    if (pending.stencil_capture == UINT32_MAX) {
      stencil_buffers_valid = false;
    }
    for (uint32_t capture : {pending.stencil_capture, pending.verify_stencil_capture}) {
      if (capture != UINT32_MAX && (capture >= scaled_stencil_captures_.size() ||
                                    scaled_stencil_captures_[capture].size < stencil_bytes)) {
        stencil_buffers_valid = false;
      }
    }
  }
  bool unorm = !pending.is_depth && pending.unorm_packing != UINT32_MAX;
  if (stencil_buffers_valid &&
      (pending.is_depth ? (pending.depth_float_view ? EnsureScaledMemoryWritebackDepthFloatPipeline()
                                                    : EnsureScaledMemoryWritebackDepthPipeline())
       : unorm          ? EnsureScaledMemoryWritebackUnormPipeline()
                        : EnsureScaledMemoryWritebackPipeline())) {
    const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    uint32_t bytes_per_block_log2 =
        (pending.memory_flags & kNativeResolveFlagMemory64bpp) ? 3 : 2;
    uint32_t dest_range_unscaled =
        pending.extent_start + pending.extent_length - pending.dest_base;
    uint64_t dest_base_scaled, dest_range_scaled, dest_use_start_scaled, dest_use_length_scaled;
    VkDescriptorSet memory_descriptor_set = VK_NULL_HANDLE;
    if (texture_cache.GetScaledResolveRange(pending.dest_base, dest_range_unscaled,
                                            bytes_per_block_log2, dest_base_scaled,
                                            dest_range_scaled) &&
        texture_cache.GetScaledResolveRange(pending.extent_start, pending.extent_length,
                                            bytes_per_block_log2, dest_use_start_scaled,
                                            dest_use_length_scaled) &&
        texture_cache.CommitScaledResolveRange(pending.dest_base, dest_range_unscaled,
                                               bytes_per_block_log2)) {
      memory_descriptor_set = command_processor_.AllocateSingleTransientDescriptor(
          VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferCompute);
    }
    VkDescriptorSet stencil_descriptor_set = VK_NULL_HANDLE;
    VkDescriptorSet stencil_verify_descriptor_set = VK_NULL_HANDLE;
    if (pending.is_depth && memory_descriptor_set != VK_NULL_HANDLE) {
      stencil_descriptor_set = command_processor_.AllocateSingleTransientDescriptor(
          VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferCompute);
      stencil_verify_descriptor_set = command_processor_.AllocateSingleTransientDescriptor(
          VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferCompute);
      if (stencil_descriptor_set == VK_NULL_HANDLE ||
          stencil_verify_descriptor_set == VK_NULL_HANDLE) {
        memory_descriptor_set = VK_NULL_HANDLE;
      }
    }
    // Sampled image descriptor sets of completed write-backs can be reused.
    uint64_t completed_submission = command_processor_.GetCompletedSubmission();
    while (!scaled_memory_writeback_descriptors_.empty() &&
           scaled_memory_writeback_descriptors_.front().first <= completed_submission) {
      descriptor_set_pool_sampled_image_->Free(scaled_memory_writeback_descriptors_.front().second);
      scaled_memory_writeback_descriptors_.pop_front();
    }
    size_t source_descriptor_index = descriptor_set_pool_sampled_image_->Allocate();
    if (memory_descriptor_set != VK_NULL_HANDLE && source_descriptor_index != SIZE_MAX) {
      VkDescriptorSet source_descriptor_set =
          descriptor_set_pool_sampled_image_->Get(source_descriptor_index);
      scaled_memory_writeback_descriptors_.emplace_back(command_processor_.GetCurrentSubmission(),
                                                        source_descriptor_index);
      VkImageView source_view = texture_cache.BeginScaledMemoryWriteback(pending.texture);
      VkDescriptorImageInfo source_image_info;
      source_image_info.sampler = VK_NULL_HANDLE;
      source_image_info.imageView = source_view;
      source_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkDescriptorBufferInfo memory_buffer_info;
      memory_buffer_info.buffer = texture_cache.scaled_resolve_buffer();
      memory_buffer_info.offset = dest_base_scaled;
      memory_buffer_info.range = dest_range_scaled;
      VkDescriptorBufferInfo stencil_buffer_info, stencil_verify_buffer_info;
      if (pending.is_depth) {
        stencil_buffer_info.buffer = scaled_stencil_captures_[pending.stencil_capture].buffer;
        stencil_buffer_info.offset = 0;
        stencil_buffer_info.range = VK_WHOLE_SIZE;
        // Without verification, bound but not read.
        stencil_verify_buffer_info = stencil_buffer_info;
        if (pending.verify_stencil_capture != UINT32_MAX) {
          stencil_verify_buffer_info.buffer =
              scaled_stencil_captures_[pending.verify_stencil_capture].buffer;
          stencil_verify_buffer_info.offset = 0;
          stencil_verify_buffer_info.range = VK_WHOLE_SIZE;
        }
      }
      VkWriteDescriptorSet writes[4];
      for (VkWriteDescriptorSet& write : writes) {
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.pNext = nullptr;
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.pImageInfo = nullptr;
        write.pBufferInfo = nullptr;
        write.pTexelBufferView = nullptr;
      }
      writes[0].dstSet = source_descriptor_set;
      writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[0].pImageInfo = &source_image_info;
      writes[1].dstSet = memory_descriptor_set;
      writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[1].pBufferInfo = &memory_buffer_info;
      writes[2].dstSet = stencil_descriptor_set;
      writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[2].pBufferInfo = &stencil_buffer_info;
      writes[3].dstSet = stencil_verify_descriptor_set;
      writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[3].pBufferInfo = &stencil_verify_buffer_info;
      dfn.vkUpdateDescriptorSets(device, pending.is_depth ? 4 : 2, writes, 0, nullptr);

      texture_cache.UseScaledResolveBufferForWrite(dest_use_start_scaled, dest_use_length_scaled);
      command_processor_.EndRenderPass();
      command_processor_.SubmitBarriers(true);
      DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
      VkPipelineLayout pipeline_layout = pending.is_depth
                                             ? scaled_memory_writeback_depth_pipeline_layout_
                                             : scaled_memory_writeback_pipeline_layout_;
      command_processor_.BindExternalComputePipeline(
          pending.is_depth ? (pending.depth_float_view ? scaled_memory_writeback_depth_float_pipeline_
                                                       : scaled_memory_writeback_depth_pipeline_)
          : unorm          ? scaled_memory_writeback_unorm_pipeline_
                           : scaled_memory_writeback_pipeline_);
      VkDescriptorSet descriptor_sets[] = {source_descriptor_set, memory_descriptor_set,
                                           stencil_descriptor_set, stencil_verify_descriptor_set};
      command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0,
                                             pending.is_depth ? 4 : 2, descriptor_sets, 0,
                                             nullptr);
      uint32_t scale_x = draw_resolution_scale_x(), scale_y = draw_resolution_scale_y();
      ScaledMemoryWritebackConstants constants;
      constants.origin_x = pending.x0 * scale_x;
      constants.origin_y = pending.y0 * scale_y;
      constants.size_x = (pending.x1 - pending.x0) * scale_x;
      constants.size_y = (pending.y1 - pending.y0) * scale_y;
      constants.flags = pending.memory_flags;
      constants.dest_pitch_texels = pending.dest_pitch_texels;
      uint32_t scale_x_log2 = uint32_t(std::countr_zero(scale_x));
      uint32_t scale_y_log2 = uint32_t(std::countr_zero(scale_y));
      bool powers_of_two = (scale_x & (scale_x - 1)) == 0 && (scale_y & (scale_y - 1)) == 0;
      constants.resolution_scale = scale_x | (scale_y << 8) | (scale_x_log2 << 16) |
                                   (scale_y_log2 << 20) | (powers_of_two ? (1u << 31) : 0u);
      constants.depth_flags = unorm ? pending.unorm_packing : pending.flags;
      if (unorm && texture_cache.IsTextureContentRedBlueSwapped(pending.texture)) {
        // An image taken over from a render target, in its order.
        constants.depth_flags |= 4;
      }
      if (pending.stencil_capture_quads) {
        constants.depth_flags |= 256;
      }
      if (pending.verify_stencil_capture != UINT32_MAX) {
        constants.depth_flags |= 512;
      }
      command_buffer.CmdVkPushConstants(pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                        sizeof(constants), &constants);
      command_buffer.CmdVkDispatch((constants.size_x + 7) / 8, (constants.size_y + 7) / 8, 1);
      written = true;
    } else if (source_descriptor_index != SIZE_MAX) {
      descriptor_set_pool_sampled_image_->Free(source_descriptor_index);
    }
  }
  if (!written) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: failed to write resolved data back to the scaled resolve "
        "memory at {:08X}",
        pending.extent_start);
  }
  DropPendingScaledResolveMemory(pending);
}

void VulkanRenderTargetCache::PrepareScaledResolveMemoryForResolve(uint32_t start,
                                                                   uint32_t length) {
  uint64_t end = uint64_t(start) + length;
  for (size_t i = 0; i < pending_scaled_resolve_memory_.size();) {
    PendingScaledResolveMemory& pending = pending_scaled_resolve_memory_[i];
    uint64_t pending_end = uint64_t(pending.extent_start) + pending.extent_length;
    if (pending.extent_start >= end || pending_end <= start) {
      ++i;
      continue;
    }
    if (start <= pending.extent_start && end >= pending_end) {
      // Replaced entirely by the new resolve.
      DropPendingScaledResolveMemory(pending);
    } else {
      WriteBackPendingScaledResolveMemory(pending);
    }
    pending_scaled_resolve_memory_.erase(pending_scaled_resolve_memory_.begin() + i);
  }
}

void VulkanRenderTargetCache::FlushPendingScaledResolveMemory(uint32_t start, uint32_t length) {
  uint64_t end = uint64_t(start) + length;
  for (size_t i = 0; i < pending_scaled_resolve_memory_.size();) {
    const PendingScaledResolveMemory& pending = pending_scaled_resolve_memory_[i];
    uint64_t pending_end = uint64_t(pending.extent_start) + pending.extent_length;
    if (pending.extent_start >= end || pending_end <= start) {
      ++i;
      continue;
    }
    WriteBackPendingScaledResolveMemory(pending);
    pending_scaled_resolve_memory_.erase(pending_scaled_resolve_memory_.begin() + i);
  }
}

void VulkanRenderTargetCache::FlushAllPendingScaledResolveMemory() {
  for (const PendingScaledResolveMemory& pending : pending_scaled_resolve_memory_) {
    WriteBackPendingScaledResolveMemory(pending);
  }
  pending_scaled_resolve_memory_.clear();
}

bool VulkanRenderTargetCache::CanResolveCopyFree(const NativeResolvePlan& plan,
                                                 VulkanTextureCache& texture_cache) const {
  if (!REXCVAR_GET(native_resolve_copy_free) || REXCVAR_GET(native_resolve_debug_reload) ||
      plan.shader != NativeResolveShader::kColorUnorm || plan.target_count != 1 ||
      !plan.targets[0].texture || plan.memory_only || !plan.source || plan.x0 || plan.y0) {
    return false;
  }
  RenderTargetKey key = plan.source->key();
  if (key.is_depth || key.msaa_samples != xenos::MsaaSamples::k1X || key.original_resolution) {
    return false;
  }
  // The resolve copies the whole image into the whole texture.
  uint32_t image_width = key.GetWidth() * GetRenderTargetScaleX(key);
  uint32_t image_height =
      plan.source->tile_rows() * xenos::kEdramTileHeightSamples * GetRenderTargetScaleY(key);
  uint32_t texture_width, texture_height;
  return texture_cache.GetCopyFreeResolveTargetExtent(plan.targets[0].texture, texture_width,
                                                      texture_height) &&
         texture_width == image_width && texture_height == image_height &&
         plan.x1 * GetRenderTargetScaleX(key) == image_width &&
         plan.y1 * GetRenderTargetScaleY(key) == image_height;
}

void VulkanRenderTargetCache::ProcessPendingCopyFreeResolve(
    const Shader* vertex_shader, bool is_rasterization_done,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask) {
  if (!pending_copy_free_resolve_.active) {
    return;
  }
  pending_copy_free_resolve_.active = false;
  const NativeResolvePlan& plan = pending_copy_free_resolve_.plan;
  VulkanTextureCache& texture_cache = *pending_copy_free_resolve_.texture_cache;
  if (vertex_shader && is_rasterization_done &&
      TryCopyFreeResolveExchange(plan, texture_cache, *vertex_shader, normalized_depth_control,
                                 normalized_color_mask)) {
    return;
  }
  if (RtDebugLogActive()) {
    REXGPU_INFO("[rt-debug] held back resolve to {:08X} copied{}", plan.dest_base,
                vertex_shader ? "" : " before another operation");
  }
  // The held back copy, with the render target still holding the data.
  PerformNativeResolve(texture_cache, plan, false);
}

bool VulkanRenderTargetCache::TryCopyFreeResolveExchange(
    const NativeResolvePlan& plan, VulkanTextureCache& texture_cache, const Shader& vertex_shader,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask) {
  VulkanRenderTarget& render_target = *plan.source;
  RenderTargetKey key = render_target.key();
  // The draw must write only this render target, through color slot 0 (what
  // Update will bind, without any conflicts to resolve), and all of it.
  if (normalized_depth_control.z_enable || normalized_depth_control.stencil_enable ||
      (normalized_color_mask & 0b1111) != 0b1111 || (normalized_color_mask >> 4)) {
    return false;
  }
  const RegisterFile& regs = register_file();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  if (rb_surface_info.msaa_samples != key.msaa_samples ||
      (rb_surface_info.surface_pitch + (xenos::kEdramTileWidthSamples - 1)) /
              xenos::kEdramTileWidthSamples !=
          key.pitch_tiles_at_32bpp) {
    return false;
  }
  auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]);
  // The resource format Update keys the render target with.
  xenos::ColorRenderTargetFormat draw_format =
      xenos::GetStorageColorFormat(color_info.color_format);
  if (draw_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
      !IsGammaFormatHostStorageSeparate()) {
    draw_format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
  }
  if (color_info.color_base != key.base_tiles || draw_format != key.GetColorFormat()) {
    return false;
  }
  Transfer::Rectangle rectangle;
  if (!(GetDrawOverwrittenRenderTargets(normalized_depth_control, normalized_color_mask,
                                        vertex_shader, rectangle, nullptr, 0b10) &
        0b10)) {
    return false;
  }
  uint32_t scale_x = GetRenderTargetScaleX(key), scale_y = GetRenderTargetScaleY(key);
  uint32_t image_width = key.GetWidth() * scale_x;
  uint32_t image_height = render_target.tile_rows() * xenos::kEdramTileHeightSamples * scale_y;
  if (rectangle.x_pixels || rectangle.y_pixels || rectangle.width_pixels * scale_x < image_width ||
      rectangle.height_pixels * scale_y < image_height) {
    return false;
  }
  void* texture = plan.targets[0].texture;
  uint32_t texture_width, texture_height;
  if (!texture_cache.GetCopyFreeResolveTargetExtent(texture, texture_width, texture_height) ||
      texture_width != image_width || texture_height != image_height) {
    return false;
  }
  // The texture's image only has the render target's format (no separate
  // transfer view format).
  if (GetColorOwnershipTransferVulkanFormat(key.GetColorFormat(), key.msaa_samples) !=
      GetColorVulkanFormat(key.GetColorFormat())) {
    return false;
  }

  // The render target's views of the texture's image, before anything changes.
  VulkanRenderTarget::Image new_image;
  VkExtent2D extent = {image_width, image_height};
  if (!CreateRenderTargetImageViewsForKey(key, texture_cache.GetTextureImage(texture), extent,
                                          new_image)) {
    return false;
  }
  command_processor_.EndRenderPass();
  // The texture takes the render target's image, with the data of the resolve
  // (red and blue in the render target's order if the resolve swaps them).
  VkImage image = render_target.image();
  VkDeviceMemory memory = render_target.memory();
  VkPipelineStageFlags stage_mask = render_target.current_stage_mask();
  VkAccessFlags access_mask = render_target.current_access_mask();
  VkImageLayout layout = render_target.current_layout();
  texture_cache.ExchangeCopyFreeResolveTargetImage(
      texture, image, memory, stage_mask, access_mask, layout,
      (plan.flags & kNativeResolveFlagSwapRedBlue) != 0);
  // The render target takes the texture's old image, which the draw overwrites.
  new_image.image = image;
  new_image.memory = memory;
  new_image.tile_rows = render_target.tile_rows();
  VulkanRenderTarget::Image old_image = render_target.ReplaceImage(new_image);
  render_target.SetUsage(stage_mask, access_mask, layout);
  // Only the views and the descriptor of the old image are the render
  // target's to destroy now.
  old_image.image = VK_NULL_HANDLE;
  old_image.memory = VK_NULL_HANDLE;
  RetiredRenderTargetImage& retired = retired_render_target_images_.emplace_back();
  retired.submission = command_processor_.GetCurrentSubmission();
  retired.is_depth = false;
  retired.image = old_image;
  RetireFramebuffersOfRenderTarget(key);
  ++copy_free_resolve_count_;
  if (REXCVAR_GET(native_resolve_copy_free_debug_writeback)) {
    for (const PendingScaledResolveMemory& pending : pending_scaled_resolve_memory_) {
      if (pending.texture == texture) {
        uint32_t start = pending.extent_start, length = pending.extent_length;
        FlushPendingScaledResolveMemory(start, length);
        texture_cache.MarkRangeAsResolved(start, length);
        break;
      }
    }
  }
  if (RtDebugLogActive()) {
    REXGPU_INFO("[rt-debug] resolve to {:08X} done by taking over the render target's image",
                plan.dest_base);
  }
  return true;
}

void VulkanRenderTargetCache::RetireFramebuffersOfRenderTarget(RenderTargetKey key) {
  uint64_t submission = command_processor_.GetCurrentSubmission();
  for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
    const FramebufferKey& framebuffer_key = it->first;
    uint32_t used = framebuffer_key.render_pass_key.depth_and_color_used;
    uint32_t bases[1 + xenos::kMaxColorRenderTargets] = {
        framebuffer_key.depth_base_tiles, framebuffer_key.color_0_base_tiles,
        framebuffer_key.color_1_base_tiles, framebuffer_key.color_2_base_tiles,
        framebuffer_key.color_3_base_tiles};
    bool uses = false;
    if (framebuffer_key.pitch_tiles_at_32bpp == key.pitch_tiles_at_32bpp &&
        bool(framebuffer_key.original_resolution) == bool(key.original_resolution)) {
      for (uint32_t i = key.is_depth ? 0 : 1;
           i < (key.is_depth ? 1 : 1 + xenos::kMaxColorRenderTargets); ++i) {
        if ((used & (uint32_t(1) << i)) && bases[i] == key.base_tiles) {
          uses = true;
        }
      }
    }
    if (uses) {
      retired_framebuffers_.emplace_back(submission, it->second.framebuffer);
      it = framebuffers_.erase(it);
    } else {
      ++it;
    }
  }
  last_update_framebuffer_ = nullptr;
}

bool VulkanRenderTargetCache::PrepareNativeResolve(const draw_util::ResolveInfo& resolve_info,
                                                   VulkanTextureCache& texture_cache,
                                                   NativeResolvePlan& plan) {
  plan.target_count = 0;
  plan.source = nullptr;
  plan.memory_only = false;
  if (!native_resolve_enabled_ || !resolve_info.copy_dest_extent_length ||
      resolve_info.copy_dest_info.copy_dest_array) {
    return false;
  }
  if (int32_t ab_frames = REXCVAR_GET(native_resolve_ab_frames); ab_frames > 0) {
    uint64_t frame = command_processor_.GetCurrentFrame();
    bool ab_on = ((frame / uint64_t(ab_frames)) & 1) == 0;
    static int ab_logged_state = -1;
    if (int(ab_on) != ab_logged_state) {
      ab_logged_state = int(ab_on);
      REXGPU_INFO("[native-resolve-ab] frame {}: {}", frame, ab_on ? "on" : "off");
    }
    if (!ab_on) {
      return false;
    }
  }
  bool is_depth = resolve_info.IsCopyingDepth();
  if (!(REXCVAR_GET(native_resolve_mask) & (is_depth ? 1 : 2))) {
    return false;
  }
  if (int32_t only = REXCVAR_GET(native_resolve_only); only >= 0) {
    static uint64_t counted_frame = UINT64_MAX;
    static int32_t resolve_index = 0;
    uint64_t frame = command_processor_.GetCurrentFrame();
    if (frame != counted_frame) {
      counted_frame = frame;
      resolve_index = 0;
    }
    if (resolve_index++ != only) {
      return false;
    }
  }
  const draw_util::ResolveEdramInfo& edram_info =
      is_depth ? resolve_info.depth_edram_info : resolve_info.color_edram_info;
  if (edram_info.msaa_samples != xenos::MsaaSamples::k1X) {
    return false;
  }
  uint32_t dest_endian = uint32_t(resolve_info.copy_dest_info.copy_dest_endian);
  if (dest_endian > uint32_t(xenos::Endian::k16in32)) {
    return false;
  }

  // The whole resolve area must be owned by one render target with the
  // surface's origin, so destination texel (x, y) is render target pixel (x, y).
  uint32_t dump_base, dump_row_length_used, dump_rows, dump_pitch;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                                 native_resolve_rectangles_);
  if (native_resolve_rectangles_.size() != 1) {
    return false;
  }
  const ResolveCopyDumpRectangle& rectangle = native_resolve_rectangles_.front();
  if (rectangle.row_first || rectangle.rows != dump_rows || rectangle.row_first_start ||
      rectangle.row_last_end != dump_row_length_used) {
    return false;
  }
  auto* source = static_cast<VulkanRenderTarget*>(rectangle.render_target);
  RenderTargetKey source_key = source->key();
  uint32_t original_base =
      is_depth ? resolve_info.depth_original_base : resolve_info.color_original_base;
  if (bool(source_key.is_depth) != is_depth || source_key.base_tiles != original_base ||
      source_key.msaa_samples != xenos::MsaaSamples::k1X ||
      source_key.GetPitchTiles() != edram_info.pitch_tiles) {
    return false;
  }

  NativeResolveShader shader;
  NativeResolvePacking packing = NativeResolvePacking::kUint32;
  uint32_t flags = 0;
  if (is_depth) {
    shader = NativeResolveShader::kDepth;
    if (source_key.GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24FS8) {
      flags |= kNativeResolveFlagDepthFloat24;
      if (depth_float24_round()) {
        flags |= kNativeResolveFlagDepthRoundToNearestEven;
      }
    }
  } else {
    auto guest_format = xenos::ColorRenderTargetFormat(edram_info.format);
    xenos::ColorRenderTargetFormat source_format = source_key.GetColorFormat();
    if (resolve_info.copy_dest_info.copy_dest_exp_bias ||
        guest_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA ||
        source_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA ||
        !xenos::IsColorResolveFormatBitwiseEquivalent(
            guest_format, xenos::ColorFormat(resolve_info.copy_dest_info.copy_dest_format))) {
      return false;
    }
    bool source_is_integer = false;
    GetColorOwnershipTransferVulkanFormat(source_format, source_key.msaa_samples,
                                          &source_is_integer);
    bool swap_supported = false;
    switch (source_format) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        if (source_is_integer) {
          return false;
        }
        shader = NativeResolveShader::kColorFloat;
        packing = NativeResolvePacking::k8888;
        swap_supported = true;
        break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
        if (source_is_integer) {
          return false;
        }
        shader = NativeResolveShader::kColorFloat;
        packing = NativeResolvePacking::k2101010;
        swap_supported = true;
        break;
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
        shader =
            source_is_integer ? NativeResolveShader::kColorUint : NativeResolveShader::kColorFloat;
        packing = source_is_integer ? NativeResolvePacking::kUint16 : NativeResolvePacking::kFloat16;
        break;
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
        shader =
            source_is_integer ? NativeResolveShader::kColorUint : NativeResolveShader::kColorFloat;
        packing = source_is_integer ? NativeResolvePacking::kUint32 : NativeResolvePacking::kFloat32;
        break;
      default:
        return false;
    }
    if (resolve_info.copy_dest_info.copy_dest_swap) {
      if (!swap_supported) {
        return false;
      }
      flags |= kNativeResolveFlagSwapRedBlue;
    }
  }

  uint32_t x0 = resolve_info.rect_x0;
  uint32_t y0 = resolve_info.rect_y0;
  uint32_t x1 = x0 + (uint32_t(resolve_info.coordinate_info.width_div_8)
                      << xenos::kResolveAlignmentPixelsLog2);
  uint32_t y1 = y0 + (resolve_info.height_div_8 << xenos::kResolveAlignmentPixelsLog2);
  uint32_t dest_pitch = uint32_t(resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32)
                        << xenos::kTextureTileWidthHeightLog2;
  // Original resolution render targets resolve to unscaled textures and memory.
  bool scaled = IsDrawResolutionScaled() && !source_key.original_resolution;
  VulkanTextureCache::NativeResolveTarget found[VulkanTextureCache::kMaxNativeResolveTargets];
  uint32_t found_count = texture_cache.FindNativeResolveTargets(
      resolve_info.copy_dest_base_raw, dest_pitch,
      xenos::TextureFormat(resolve_info.copy_dest_info.copy_dest_format),
      xenos::Endian(dest_endian), scaled, found);
  bool memory_only_all = REXCVAR_GET(native_resolve_debug_memory_only_all);
  // Textures written through a unorm view of their own format
  // (native_resolve_unorm_views) are exact targets only for a float resolve of
  // a source of the same format.
  bool unorm_targets_allowed =
      shader == NativeResolveShader::kColorFloat &&
      (packing == NativeResolvePacking::k8888 || packing == NativeResolvePacking::k2101010) &&
      !source_key.is_depth;
  VkFormat source_vulkan_format =
      unorm_targets_allowed ? GetColorVulkanFormat(source_key.GetColorFormat()) : VK_FORMAT_UNDEFINED;
  bool any_own_format_target = false;
  for (uint32_t i = 0; i < found_count && !memory_only_all; ++i) {
    if (found[i].width > x0 && found[i].height > y0) {
      if (VulkanTextureCache::IsNativeResolveOwnFormatView(found[i].format)) {
        // Depth textures get depth only, unorm ones a float resolve of a source
        // of the same format.
        if (is_depth ? found[i].format != VK_FORMAT_R32_SFLOAT
                     : (!unorm_targets_allowed || found[i].format != source_vulkan_format)) {
          continue;
        }
        any_own_format_target = true;
      }
      plan.targets[plan.target_count++] = found[i];
    }
  }
  if (any_own_format_target) {
    // All targets of a destination format are created the same way, so a raw
    // integer view target doesn't appear next to an own format one.
    for (uint32_t i = 0; i < plan.target_count; ++i) {
      if (!VulkanTextureCache::IsNativeResolveOwnFormatView(plan.targets[i].format)) {
        return false;
      }
    }
    shader = is_depth ? NativeResolveShader::kDepthFloat : NativeResolveShader::kColorUnorm;
  }
  if (!plan.target_count) {
    if (!REXCVAR_GET(native_resolve_memory_only) && !memory_only_all) {
      return false;
    }
    // A stand-in target covering the rectangle, with no image: the draw only
    // writes the memory, which textures sampling the destination later load.
    plan.targets[0] = VulkanTextureCache::NativeResolveTarget();
    plan.targets[0].width = x1;
    plan.targets[0].height = y1;
    plan.target_count = 1;
    plan.memory_only = true;
  }
  plan.source = source;
  plan.shader = shader;
  plan.packing = packing;
  plan.flags = flags;
  plan.x0 = x0;
  plan.y0 = y0;
  plan.x1 = x1;
  plan.y1 = y1;
  plan.dest_base = resolve_info.copy_dest_base_raw & 0x1FFFFFFF;
  plan.dest_pitch_texels = dest_pitch;
  plan.memory_flags = kNativeResolveFlagWriteMemory |
                      (dest_endian << kNativeResolveFlagMemoryEndianShift);
  if (!is_depth && xenos::IsColorRenderTargetFormat64bpp(source_key.GetColorFormat())) {
    plan.memory_flags |= kNativeResolveFlagMemory64bpp;
  }
  // With draw resolution scaling, the resolved memory is the scaled resolve
  // buffer, written by the regular resolve; the native draws then only keep
  // the textures up to date (so they aren't reloaded from that buffer).
  // With native_resolve_scaled_single_pass, the first target's draw writes the
  // scaled resolve buffer instead, through a single storage buffer descriptor
  // standing in for the shared memory (so only with one shared memory binding).
  bool can_write_memory_here =
      !scaled || (REXCVAR_GET(native_resolve_scaled_single_pass) &&
                  native_resolve_shared_memory_binding_count_ == 1);
  plan.can_write_memory =
      can_write_memory_here && plan.targets[0].width >= x1 && plan.targets[0].height >= y1;
  if (scaled) {
    plan.memory_flags |= kNativeResolveFlagMemoryScaled;
  }
  return true;
}

VulkanRenderTargetCache::NativeResolveBuffer* VulkanRenderTargetCache::AcquireNativeResolveBuffer(
    VkDeviceSize size) {
  uint64_t completed = command_processor_.GetCompletedSubmission();
  NativeResolveBuffer* result = nullptr;
  {
    auto global_lock = native_resolve_buffers_critical_region_.Acquire();
    // Buffers are immutable while a submission can still read or write them.
    // Reuse a completed buffer, preferring an invalid one and the closest size.
    for (const auto& candidate : native_resolve_buffers_) {
      if (candidate->last_submission > completed || candidate->capacity < size ||
          candidate->pending_memory) {
        continue;
      }
      if (!result || (result->valid && !candidate->valid) ||
          (result->valid == candidate->valid && candidate->capacity < result->capacity)) {
        result = candidate.get();
      }
    }
    if (result) {
      if (result->watch) {
        result->shared_memory->UnwatchMemoryRange(result->watch);
        result->watch = nullptr;
      }
      result->valid = false;
      result->last_submission = command_processor_.GetCurrentSubmission();
    }
  }
  if (result) {
    command_processor_.PushBufferMemoryBarrier(
        result->buffer, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
    return result;
  }
  VkDeviceSize capacity = rex::align(size, VkDeviceSize(64) << 10);
  uint64_t limit = uint64_t(std::clamp(REXCVAR_GET(native_resolve_buffer_mb), 1, 512)) << 20;
  if (capacity > limit || native_resolve_buffer_bytes_ > limit - capacity ||
      native_resolve_buffers_.size() >= 512) {
    return nullptr;
  }
  auto allocation = std::make_unique<NativeResolveBuffer>();
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          command_processor_.GetVulkanDevice(), capacity,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, allocation->buffer, allocation->memory)) {
    return nullptr;
  }
  allocation->capacity = capacity;
  allocation->last_submission = command_processor_.GetCurrentSubmission();
  result = allocation.get();
  native_resolve_buffers_.push_back(std::move(allocation));
  native_resolve_buffer_bytes_ += capacity;
  return result;
}

void VulkanRenderTargetCache::ClearNativeResolveBuffers() {
  // Shutdown and cache clear have already waited for all queue operations.
  auto allocations = std::move(native_resolve_buffers_);
  native_resolve_buffer_bytes_ = 0;
  {
    auto global_lock = native_resolve_buffers_critical_region_.Acquire();
    for (const auto& allocation : allocations) {
      if (allocation->watch) {
        allocation->shared_memory->UnwatchMemoryRange(allocation->watch);
        allocation->watch = nullptr;
      }
      allocation->valid = false;
    }
  }
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  for (const auto& allocation : allocations) {
    vulkan_device->functions().vkDestroyBuffer(vulkan_device->device(), allocation->buffer,
                                               nullptr);
    vulkan_device->functions().vkFreeMemory(vulkan_device->device(), allocation->memory, nullptr);
  }
}

VulkanRenderTargetCache::NativeResolveBuffer* VulkanRenderTargetCache::FindNativeResolveBufferRange(
    uint32_t address, uint32_t length, bool scaled) {
  // The caller holds native_resolve_buffers_critical_region_.
  if (!REXCVAR_GET(native_resolve_buffer_reads) || !length ||
      uint64_t(address) + length > SharedMemory::kBufferSize) {
    return nullptr;
  }
  uint64_t scale_area =
      scaled ? uint64_t(draw_resolution_scale_x()) * draw_resolution_scale_y() : 1;
  for (auto it = native_resolve_buffers_.rbegin(); it != native_resolve_buffers_.rend(); ++it) {
    NativeResolveBuffer& allocation = **it;
    if (!allocation.valid || allocation.scaled != scaled || address < allocation.extent_start ||
        uint64_t(address) + length > uint64_t(allocation.extent_start) + allocation.extent_length) {
      continue;
    }
    VkDeviceSize offset = VkDeviceSize(address - allocation.base) * scale_area;
    if (!(offset %
          command_processor_.GetVulkanDevice()->properties().minStorageBufferOffsetAlignment)) {
      return &allocation;
    }
  }
  return nullptr;
}

bool VulkanRenderTargetCache::CanUseNativeResolveBufferRange(uint32_t address, uint32_t length,
                                                             bool scaled) {
  auto global_lock = native_resolve_buffers_critical_region_.Acquire();
  return FindNativeResolveBufferRange(address, length, scaled) != nullptr;
}

bool VulkanRenderTargetCache::UseNativeResolveBufferRange(uint32_t address, uint32_t length,
                                                          VkDescriptorBufferInfo& buffer_info,
                                                          bool scaled) {
  bool found = false;
  {
    auto global_lock = native_resolve_buffers_critical_region_.Acquire();
    if (NativeResolveBuffer* allocation = FindNativeResolveBufferRange(address, length, scaled)) {
      uint64_t scale_area =
          scaled ? uint64_t(draw_resolution_scale_x()) * draw_resolution_scale_y() : 1;
      VkDeviceSize offset = VkDeviceSize(address - allocation->base) * scale_area;
      buffer_info = {allocation->buffer, offset, VkDeviceSize(length) * scale_area};
      allocation->last_submission = command_processor_.GetCurrentSubmission();
      found = true;
    }
  }
  if (!found) {
    return false;
  }
  // Includes both the padding copy and the resolve's fragment writes. The
  // buffer stays immutable until every submission using it has completed.
  command_processor_.PushBufferMemoryBarrier(
      buffer_info.buffer, buffer_info.offset, buffer_info.range, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  static uint64_t native_reads = 0;
  if (++native_reads <= 8 || !(native_reads & 255)) {
    REXGPU_INFO("[native-resolve-buffer-read] {} bytes at {:08X} ({})", length, address,
                native_reads);
  }
  return true;
}

bool VulkanRenderTargetCache::WriteBackNativeResolveBuffer(NativeResolveBuffer& allocation) {
  uint32_t address, length, base;
  {
    auto global_lock = native_resolve_buffers_critical_region_.Acquire();
    if (!allocation.pending_memory) {
      return true;
    }
    address = allocation.extent_start;
    length = allocation.extent_length;
    base = allocation.base;
  }
  uint64_t scale_area =
      allocation.scaled ? uint64_t(draw_resolution_scale_x()) * draw_resolution_scale_y() : 1;
  VkBuffer mirror;
  if (allocation.scaled) {
    if (!allocation.texture_cache->CommitScaledResolveRange(address, length)) {
      return false;
    }
    mirror = allocation.texture_cache->scaled_resolve_buffer();
  } else {
    if (!allocation.shared_memory->RequestRange(address, length)) {
      return false;
    }
    mirror = allocation.shared_memory->buffer();
  }
  std::vector<VkBufferCopy> regions;
  VkDeviceSize copy_bytes = 0;
  {
    auto global_lock = native_resolve_buffers_critical_region_.Acquire();
    if (!allocation.pending_memory) {
      return true;
    }
    if (allocation.valid) {
      regions.push_back({VkDeviceSize(address - base) * scale_area,
                         VkDeviceSize(address) * scale_area, VkDeviceSize(length) * scale_area});
    } else {
      // A CPU write invalidates the direct resource, not every GPU-written
      // page in it. RequestRange above uploads the changed CPU pages. Preserve
      // only pages still owned by the GPU so the old resolve can't overwrite
      // those uploads, including partially covered first and last pages.
      uint32_t page_size = uint32_t(rex::memory::page_size());
      uint32_t end = address + length;
      for (uint32_t cursor = address; cursor < end;) {
        uint32_t next = std::min(end, (cursor & ~(page_size - 1)) + page_size);
        if (allocation.shared_memory->IsRangeGpuWritten(cursor, next - cursor)) {
          VkDeviceSize destination = VkDeviceSize(cursor) * scale_area;
          VkDeviceSize size = VkDeviceSize(next - cursor) * scale_area;
          if (!regions.empty() && regions.back().dstOffset + regions.back().size == destination) {
            regions.back().size += size;
          } else {
            regions.push_back({VkDeviceSize(cursor - base) * scale_area, destination, size});
          }
        }
        cursor = next;
      }
    }
    allocation.pending_memory = false;
    if (regions.empty()) {
      return true;
    }
    allocation.last_submission = command_processor_.GetCurrentSubmission();
  }
  for (const auto& region : regions) {
    copy_bytes += region.size;
  }
  command_processor_.PushBufferMemoryBarrier(
      allocation.buffer, VkDeviceSize(address - base) * scale_area,
      VkDeviceSize(length) * scale_area, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  if (allocation.scaled) {
    allocation.texture_cache->UseScaledResolveBufferForWrite(VkDeviceSize(address) * scale_area,
                                                             VkDeviceSize(length) * scale_area);
  } else {
    allocation.shared_memory->Use(VulkanSharedMemory::Usage::kTransferDestination,
                                  std::make_pair(address, length));
  }
  command_processor_.SubmitBarriers(true);
  command_processor_.deferred_command_buffer().CmdVkCopyBuffer(
      allocation.buffer, mirror, uint32_t(regions.size()), regions.data());
  static uint64_t writebacks = 0;
  if (++writebacks <= 8 || !(writebacks & 255)) {
    REXGPU_INFO("[native-resolve-buffer-writeback] {} bytes at {:08X}, scaled={} ({})", copy_bytes,
                address, allocation.scaled, writebacks);
  }
  return true;
}

bool VulkanRenderTargetCache::FlushNativeResolveMemory(uint32_t address, uint32_t length,
                                                       bool scaled) {
  if (!REXCVAR_GET(native_resolve_buffer_lazy_memory) || native_resolve_memory_flushing_) {
    return true;
  }
  if (!length || address >= SharedMemory::kBufferSize) {
    return true;
  }
  uint64_t end = uint64_t(address) + std::min(length, SharedMemory::kBufferSize - address);
  native_resolve_memory_flushing_ = true;
  bool result = true;
  for (const auto& allocation : native_resolve_buffers_) {
    bool overlaps;
    {
      auto global_lock = native_resolve_buffers_critical_region_.Acquire();
      overlaps = allocation->pending_memory && allocation->scaled == scaled &&
                 address < uint64_t(allocation->extent_start) + allocation->extent_length &&
                 allocation->extent_start < end;
    }
    if (overlaps && !WriteBackNativeResolveBuffer(*allocation)) {
      result = false;
      break;
    }
  }
  native_resolve_memory_flushing_ = false;
  return result;
}

bool VulkanRenderTargetCache::PrepareNativeResolveMemoryForWrite(uint32_t address,
                                                                 uint32_t length) {
  if (!REXCVAR_GET(native_resolve_buffer_lazy_memory) || native_resolve_memory_flushing_) {
    return true;
  }
  // An input copy has already preserved the old bytes within this extent.
  // Fully replaced versions need no mirror copy. Partial overlaps preserve
  // the old version outside this write before its watch is invalidated.
  uint64_t end = uint64_t(address) + length;
  native_resolve_memory_flushing_ = true;
  bool result = true;
  for (const auto& allocation : native_resolve_buffers_) {
    bool overlaps = false;
    {
      auto global_lock = native_resolve_buffers_critical_region_.Acquire();
      if (allocation->pending_memory &&
          address < uint64_t(allocation->extent_start) + allocation->extent_length &&
          allocation->extent_start < end) {
        if (address <= allocation->extent_start &&
            end >= uint64_t(allocation->extent_start) + allocation->extent_length) {
          allocation->pending_memory = false;
        } else {
          overlaps = true;
        }
      }
    }
    if (overlaps && !WriteBackNativeResolveBuffer(*allocation)) {
      result = false;
      break;
    }
  }
  native_resolve_memory_flushing_ = false;
  return result;
}

bool VulkanRenderTargetCache::TryNativeResolveBuffer(const draw_util::ResolveInfo& resolve_info,
                                                     const NativeResolvePlan& plan,
                                                     VulkanSharedMemory& shared_memory,
                                                     VulkanTextureCache& texture_cache,
                                                     bool source_original_resolution) {
  if (!REXCVAR_GET(native_resolve_buffers) || GetPath() != Path::kHostRenderTargets ||
      !native_resolve_shared_memory_binding_count_) {
    return false;
  }
  uint32_t extent_start = resolve_info.copy_dest_extent_start;
  uint32_t extent_length = resolve_info.copy_dest_extent_length;
  uint64_t extent_end = uint64_t(extent_start) + extent_length;
  if (!extent_length || ((plan.dest_base | extent_start | extent_length) & 3) ||
      extent_start < plan.dest_base || extent_end > SharedMemory::kBufferSize) {
    return false;
  }
  // The shader retains the tiled surface coordinates, with the physical base
  // removed. Keep the allocation in the first descriptor array element even
  // on devices that split the guest memory mirror into several descriptors.
  bool scaled = (plan.memory_flags & kNativeResolveFlagMemoryScaled) != 0;
  uint64_t scale_area =
      scaled ? uint64_t(draw_resolution_scale_x()) * draw_resolution_scale_y() : 1;
  VkDeviceSize buffer_size = (extent_end - plan.dest_base) * scale_area;
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  if (buffer_size > vulkan_device->properties().maxStorageBufferRange ||
      buffer_size > SharedMemory::kBufferSize / native_resolve_shared_memory_binding_count_) {
    return false;
  }
  bool lazy_memory =
      REXCVAR_GET(native_resolve_buffer_lazy_memory) && REXCVAR_GET(native_resolve_buffer_reads);
  VkDescriptorBufferInfo initial_native_info;
  bool native_initial = lazy_memory && UseNativeResolveBufferRange(extent_start, extent_length,
                                                                   initial_native_info, scaled);
  VkBuffer mirror;
  if (scaled) {
    if (!native_initial && !texture_cache.CommitScaledResolveRange(
                               plan.dest_base, uint32_t(extent_end - plan.dest_base))) {
      return false;
    }
    mirror = texture_cache.scaled_resolve_buffer();
  } else {
    if (!native_initial && !shared_memory.RequestRange(extent_start, extent_length)) {
      return false;
    }
    mirror = shared_memory.buffer();
  }
  NativeResolveBuffer* allocation = nullptr;
  if (lazy_memory && native_initial && REXCVAR_GET(native_resolve_buffer_reuse)) {
    auto global_lock = native_resolve_buffers_critical_region_.Acquire();
    for (const auto& candidate : native_resolve_buffers_) {
      if (candidate->valid && candidate->buffer == initial_native_info.buffer &&
          candidate->base == plan.dest_base && candidate->extent_start == extent_start &&
          candidate->extent_length == extent_length && candidate->scaled == scaled) {
        allocation = candidate.get();
        break;
      }
    }
  }
  bool reusing = allocation != nullptr;
  if (!allocation) {
    allocation = AcquireNativeResolveBuffer(buffer_size);
  }
  if (!allocation) {
    return false;
  }
  VkDescriptorSet descriptor_set = command_processor_.AllocateSingleTransientDescriptor(
      VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferNativeVertexStreams);
  if (descriptor_set == VK_NULL_HANDLE) {
    return false;
  }
  std::array<VkDescriptorBufferInfo, 4> buffer_infos;
  assert_true(native_resolve_shared_memory_binding_count_ <= buffer_infos.size());
  for (uint32_t i = 0; i < native_resolve_shared_memory_binding_count_; ++i) {
    buffer_infos[i] = {allocation->buffer, 0, buffer_size};
  }
  VkWriteDescriptorSet descriptor_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  descriptor_write.dstSet = descriptor_set;
  descriptor_write.dstBinding = 0;
  descriptor_write.descriptorCount = native_resolve_shared_memory_binding_count_;
  descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_write.pBufferInfo = buffer_infos.data();
  vulkan_device->functions().vkUpdateDescriptorSets(vulkan_device->device(), 1, &descriptor_write,
                                                    0, nullptr);

  // A tiled extent includes padding outside the drawn rectangle. Preserve it,
  // rather than overwriting adjacent contents when copying the result back.
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  VkBufferCopy copy_region = {};
  copy_region.srcOffset = VkDeviceSize(extent_start) * scale_area;
  copy_region.dstOffset = VkDeviceSize(extent_start - plan.dest_base) * scale_area;
  copy_region.size = VkDeviceSize(extent_length) * scale_area;
  VkBuffer initial_buffer = mirror;
  if (reusing) {
    // These bytes are written by GPU commands, never mapped CPU writes. A
    // dependency orders earlier submissions and texture loads before this
    // resolve; padding remains intact without copying the buffer to itself.
    command_processor_.PushBufferMemoryBarrier(
        allocation->buffer, 0, buffer_size, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  } else if (native_initial) {
    initial_buffer = initial_native_info.buffer;
    copy_region.srcOffset = initial_native_info.offset;
    command_processor_.PushBufferMemoryBarrier(
        initial_buffer, initial_native_info.offset, initial_native_info.range,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED, false);
  } else if (scaled) {
    texture_cache.UseScaledResolveBufferForRead(std::make_pair(extent_start, extent_length));
  } else {
    shared_memory.Use(VulkanSharedMemory::Usage::kRead, {},
                      std::make_pair(extent_start, extent_length));
  }
  if (!reusing) {
    command_processor_.SubmitBarriers(true);
    command_buffer.CmdVkCopyBuffer(initial_buffer, allocation->buffer, 1, &copy_region);
    command_processor_.PushBufferMemoryBarrier(
        allocation->buffer, 0, buffer_size, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  }
  if (!PrepareNativeResolveMemoryForWrite(extent_start, extent_length)) {
    return false;
  }
  {
    // Arm the new watch in the same critical region that invalidates earlier
    // snapshots. A CPU write between the resolve and a texture load must never
    // make that load reuse old GPU bytes.
    auto global_lock = native_resolve_buffers_critical_region_.Acquire();
    texture_cache.MarkRangeAsResolved(extent_start, extent_length, source_original_resolution);
    allocation->base = plan.dest_base;
    allocation->extent_start = extent_start;
    allocation->extent_length = extent_length;
    allocation->scaled = scaled;
    allocation->shared_memory = &shared_memory;
    allocation->texture_cache = &texture_cache;
    allocation->watch = shared_memory.WatchMemoryRange(
        extent_start, extent_length,
        [](const std::unique_lock<std::recursive_mutex>&, void*, void* data, uint64_t,
           bool invalidated_by_gpu) {
          auto& buffer = *static_cast<NativeResolveBuffer*>(data);
          buffer.valid = false;
          // A CPU write invalidates direct reads of the whole resource, but
          // other pages still contain GPU-written data. Keep the pending
          // writeback so it can preserve those pages alongside new CPU data.
          // GPU writes already preserve partial overlaps before invalidation.
          if (invalidated_by_gpu) {
            buffer.pending_memory = false;
          }
          buffer.watch = nullptr;
        },
        this, allocation, 0);
    allocation->valid = allocation->watch != nullptr;
  }
  PerformNativeResolve(texture_cache, plan, true, descriptor_set, VK_NULL_HANDLE,
                       scaled ? 0 : plan.dest_base >> 2);

  {
    auto global_lock = native_resolve_buffers_critical_region_.Acquire();
    lazy_memory = lazy_memory && allocation->valid;
    allocation->pending_memory = lazy_memory;
  }
  if (lazy_memory) {
    static uint64_t deferred = 0;
    if (++deferred <= 8 || !(deferred & 255)) {
      REXGPU_INFO("[native-resolve-buffer-deferred] {} bytes at {:08X}, scaled={} ({})",
                  VkDeviceSize(extent_length) * scale_area, extent_start, scaled, deferred);
    }
    if (reusing) {
      static uint64_t reused = 0;
      if (++reused <= 8 || !(reused & 255)) {
        REXGPU_INFO("[native-resolve-buffer-reuse] {} bytes at {:08X} ({})", extent_length,
                    extent_start, reused);
      }
    }
    return true;
  }

  // Both the untouched padding and the resolved texels are copied back. The
  // transfer reads therefore depend on the original copy and the draw writes.
  command_processor_.PushBufferMemoryBarrier(
      allocation->buffer, 0, buffer_size,
      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  if (scaled) {
    if (!texture_cache.CommitScaledResolveRange(plan.dest_base,
                                                uint32_t(extent_end - plan.dest_base))) {
      return false;
    }
    texture_cache.UseScaledResolveBufferForWrite(VkDeviceSize(extent_start) * scale_area,
                                                 VkDeviceSize(extent_length) * scale_area);
  } else {
    shared_memory.Use(VulkanSharedMemory::Usage::kTransferDestination,
                      std::make_pair(extent_start, extent_length));
  }
  command_processor_.SubmitBarriers(true);
  copy_region.srcOffset = VkDeviceSize(extent_start - plan.dest_base) * scale_area;
  copy_region.dstOffset = VkDeviceSize(extent_start) * scale_area;
  command_buffer.CmdVkCopyBuffer(allocation->buffer, mirror, 1, &copy_region);
  static uint64_t resolve_buffers = 0;
  if (++resolve_buffers <= 8 || !(resolve_buffers & 255)) {
    REXGPU_INFO(
        "[native-resolve-buffer] {} bytes at {:08X}, {} bytes allocated, scaled={}, "
        "compatibility copy-back ({})",
        extent_length, extent_start, buffer_size, scaled, resolve_buffers);
  }
  return true;
}

bool VulkanRenderTargetCache::TryNativeResolveImageCopy(VulkanTextureCache& texture_cache,
                                                        const NativeResolvePlan& plan) {
  if (!REXCVAR_GET(native_resolve_image_copies) || plan.memory_only ||
      plan.shader != NativeResolveShader::kColorUnorm || plan.x0 || plan.y0 ||
      (plan.flags & ~kNativeResolveFlagSwapRedBlue)) {
    return false;
  }
  VulkanRenderTarget& source = *plan.source;
  RenderTargetKey key = source.key();
  if (key.is_depth || key.msaa_samples != xenos::MsaaSamples::k1X) {
    return false;
  }
  VkFormat format = GetColorVulkanFormat(key.GetColorFormat());
  uint32_t source_height = source.tile_rows() * xenos::kEdramTileHeightSamples;
  // A view swizzle applies to the whole image, so every target must be fully
  // replaced. Partial resolves keep the draw path and its existing order.
  for (uint32_t i = 0; i < plan.target_count; ++i) {
    const auto& target = plan.targets[i];
    if (!target.texture || target.format != format || !target.width || !target.height ||
        target.width > plan.x1 || target.height > plan.y1 || target.width > key.GetWidth() ||
        target.height > source_height ||
        texture_cache.GetTextureImage(target.texture) == source.image()) {
      return false;
    }
  }
  command_processor_.EndRenderPass();
  command_processor_.PushImageMemoryBarrier(
      source.image(), ui::vulkan::util::InitializeSubresourceRange(), source.current_stage_mask(),
      VK_PIPELINE_STAGE_TRANSFER_BIT, source.current_access_mask(), VK_ACCESS_TRANSFER_READ_BIT,
      source.current_layout(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  source.SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  for (uint32_t i = 0; i < plan.target_count; ++i) {
    texture_cache.BeginNativeResolveWrite(plan.targets[i], true);
  }
  command_processor_.SubmitBarriers(true);
  auto& command_buffer = command_processor_.deferred_command_buffer();
  bool swapped = (plan.flags & kNativeResolveFlagSwapRedBlue) != 0;
  bool debug_reload = REXCVAR_GET(native_resolve_debug_reload);
  for (uint32_t i = 0; i < plan.target_count; ++i) {
    const auto& target = plan.targets[i];
    VkImageCopy copy = {};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {target.width * GetRenderTargetScaleX(key),
                   target.height * GetRenderTargetScaleY(key), 1};
    command_buffer.CmdVkCopyImage(source.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  texture_cache.GetTextureImage(target.texture),
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    texture_cache.SetTextureContentRedBlueSwapped(target.texture, swapped);
    if (!debug_reload) {
      texture_cache.EndNativeResolveWrite(target);
    }
  }
  native_resolve_texture_write_count_ += plan.target_count;
  static thread_local uint64_t copied = 0;
  if (++copied <= 8 || !(copied & 4095)) {
    REXGPU_INFO("[native-resolve-image-copy] {} resolves copied into separate images", copied);
  }
  if (RtDebugLogActive()) {
    REXGPU_INFO("[rt-debug] native resolve copied {} separate texture images", plan.target_count);
  }
  return true;
}

void VulkanRenderTargetCache::PerformNativeResolve(VulkanTextureCache& texture_cache,
                                                   const NativeResolvePlan& plan, bool write_memory,
                                                   VkDescriptorSet scaled_memory_descriptor_set,
                                                   VkDescriptorSet stencil_capture_descriptor_set,
                                                   uint32_t memory_base_dwords) {
  if (!plan.source || !plan.target_count) {
    return;
  }
  VulkanRenderTarget& source = *plan.source;
  bool is_depth = IsNativeResolveDepthShader(plan.shader);

  VulkanGpuProfiler& gpu_profiler = command_processor_.gpu_profiler();
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  if (gpu_profiler.per_draw()) {
    // Key: shader, memory writes (bit 52, scaled 48), targets and destination;
    // rectangle size and first target format.
    gpu_profiler.MarkKeyed(
        command_buffer, VulkanGpuProfiler::Category::kNativeResolve,
        (uint64_t(plan.shader) << 56) | (uint64_t(write_memory) << 52) |
            (uint64_t(scaled_memory_descriptor_set != VK_NULL_HANDLE) << 48) |
            (uint64_t(plan.target_count) << 40) | plan.dest_base,
        (uint64_t(plan.x1 - plan.x0) << 48) | (uint64_t(plan.y1 - plan.y0) << 32) |
            uint32_t(plan.targets[0].format));
  } else {
    gpu_profiler.Mark(command_buffer, VulkanGpuProfiler::Category::kNativeResolve);
  }

  if (!write_memory && stencil_capture_descriptor_set == VK_NULL_HANDLE &&
      TryNativeResolveImageCopy(texture_cache, plan)) {
    gpu_profiler.Mark(command_buffer, VulkanGpuProfiler::Category::kResolve);
    return;
  }

  // Usually already readable after being dumped for the resolve.
  VkPipelineStageFlags source_stage_mask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  if (source.current_layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
      source.current_access_mask() == VK_ACCESS_SHADER_READ_BIT) {
    source_stage_mask |= source.current_stage_mask();
  }
  command_processor_.PushImageMemoryBarrier(
      source.image(),
      ui::vulkan::util::InitializeSubresourceRange(
          is_depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                   : VK_IMAGE_ASPECT_COLOR_BIT),
      source.current_stage_mask(), source_stage_mask, source.current_access_mask(),
      VK_ACCESS_SHADER_READ_BIT, source.current_layout(),
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  source.SetUsage(source_stage_mask, VK_ACCESS_SHADER_READ_BIT,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  // The layouts are created with the first pipeline, which may not have been
  // looked up yet (binding a null layout crashes some drivers).
  if (!EnsureNativeResolvePipelineLayouts()) {
    return;
  }
  VkPipelineLayout pipeline_layout = is_depth ? native_resolve_pipeline_layout_depth_
                                              : native_resolve_pipeline_layout_color_;
  bool capture_stencil = !write_memory && stencil_capture_descriptor_set != VK_NULL_HANDLE;
  VkDescriptorSet descriptor_sets[] = {
      source.GetDescriptorSetTransferSource(),
      scaled_memory_descriptor_set != VK_NULL_HANDLE ? scaled_memory_descriptor_set
      : capture_stencil ? stencil_capture_descriptor_set
                        : command_processor_.shared_memory_and_edram_descriptor_set(),
  };
  NativeResolveConstants constants;
  constants.source_offset_x = 0;
  constants.source_offset_y = 0;
  constants.packing = plan.packing;
  constants.dest_base_dwords = plan.dest_base >> 2;
  constants.dest_pitch_texels = plan.dest_pitch_texels;
  constants.memory_base_dwords = memory_base_dwords;
  {
    uint32_t scale_x = GetRenderTargetScaleX(source.key());
    uint32_t scale_y = GetRenderTargetScaleY(source.key());
    uint32_t scale_x_log2 = uint32_t(std::countr_zero(scale_x));
    uint32_t scale_y_log2 = uint32_t(std::countr_zero(scale_y));
    bool powers_of_two = (scale_x & (scale_x - 1)) == 0 && (scale_y & (scale_y - 1)) == 0;
    constants.resolution_scale = scale_x | (scale_y << 8) | (scale_x_log2 << 16) |
                                 (scale_y_log2 << 20) | (powers_of_two ? (1u << 31) : 0u);
    constants.stencil_capture_origin = (plan.x0 * scale_x) | ((plan.y0 * scale_y) << 16);
    constants.stencil_capture_pitch_quads = ((plan.x1 - plan.x0) * scale_x) >> 1;
  }

  bool written[VulkanTextureCache::kMaxNativeResolveTargets] = {};
  for (uint32_t i = 0; i < plan.target_count; ++i) {
    const VulkanTextureCache::NativeResolveTarget& target = plan.targets[i];
    uint32_t x1 = std::min(plan.x1, target.width);
    uint32_t y1 = std::min(plan.y1, target.height);
    if (x1 <= plan.x0 || y1 <= plan.y0) {
      continue;
    }
    VkPipeline pipeline = GetNativeResolvePipeline(plan.shader, target.format);
    if (pipeline == VK_NULL_HANDLE) {
      // Stays outdated, and will be reloaded from the resolved memory.
      continue;
    }
    if (target.texture) {
      texture_cache.BeginNativeResolveWrite(target);
    }
    command_processor_.EndRenderPass();
    // Guest texels to host pixels: scaled render targets and scaled textures
    // of resolved memory have the same scale, so host texel (x, y) of the
    // target is host pixel (x, y) of the source.
    uint32_t scale_x = GetRenderTargetScaleX(source.key());
    uint32_t scale_y = GetRenderTargetScaleY(source.key());
    native_resolve_framebuffer_.host_extent.width = target.width * scale_x;
    native_resolve_framebuffer_.host_extent.height = target.height * scale_y;
    command_processor_.SubmitBarriersAndEnterRenderTargetCacheRenderPass(
        VK_NULL_HANDLE, &native_resolve_framebuffer_, target.view, false);
    VkViewport viewport;
    viewport.x = float(plan.x0 * scale_x);
    viewport.y = float(plan.y0 * scale_y);
    viewport.width = float((x1 - plan.x0) * scale_x);
    viewport.height = float((y1 - plan.y0) * scale_y);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    command_processor_.SetViewport(viewport);
    VkRect2D scissor;
    scissor.offset.x = int32_t(plan.x0 * scale_x);
    scissor.offset.y = int32_t(plan.y0 * scale_y);
    scissor.extent.width = (x1 - plan.x0) * scale_x;
    scissor.extent.height = (y1 - plan.y0) * scale_y;
    command_processor_.SetScissor(scissor);
    command_processor_.BindExternalGraphicsPipeline(pipeline);
    command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0,
                                           uint32_t(rex::countof(descriptor_sets)),
                                           descriptor_sets, 0, nullptr);
    constants.flags = plan.flags;
    if (plan.shader == NativeResolveShader::kColorUnorm && target.texture &&
        texture_cache.IsTextureContentRedBlueSwapped(target.texture)) {
      constants.flags |= kNativeResolveFlagTextureSwapRedBlue;
    }
    if (write_memory && i == 0) {
      constants.flags |= plan.memory_flags;
    }
    if (capture_stencil && i == 0) {
      constants.flags |= kNativeResolveFlagStencilCapture;
    }
    command_buffer.CmdVkPushConstants(pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                      sizeof(constants), &constants);
    command_buffer.CmdVkDraw(3, 1, 0, 0);
    written[i] = true;
  }
  command_processor_.EndRenderPass();

  uint32_t written_count = 0;
  bool debug_reload = REXCVAR_GET(native_resolve_debug_reload);
  for (uint32_t i = 0; i < plan.target_count; ++i) {
    if (written[i] && plan.targets[i].texture) {
      if (!debug_reload) {
        texture_cache.EndNativeResolveWrite(plan.targets[i]);
      }
      if (plan.shader != NativeResolveShader::kColorUnorm) {
        texture_cache.ClearTextureContentRedBlueSwapped(plan.targets[i].texture);
      }
      ++written_count;
    }
  }
  native_resolve_texture_write_count_ += written_count;
  if (RtDebugLogActive()) {
    if (plan.memory_only) {
      REXGPU_INFO("[rt-debug] native resolve wrote only the memory{}",
                  written[0] ? "" : " - FAILED");
    } else {
      REXGPU_INFO("[rt-debug] native resolve wrote {} of {} textures", written_count,
                  plan.target_count);
    }
  }
  gpu_profiler.Mark(command_buffer, VulkanGpuProfiler::Category::kResolve);
}

}  // namespace rex::graphics::vulkan
