#pragma once
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

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/graphics/vulkan/texture_cache.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/ui/vulkan/single_layout_descriptor_set_pool.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;

class VulkanRenderTargetCache final : public RenderTargetCache {
 public:
  union RenderPassKey {
    struct {
      // If emulating 2x as 4x, this is still 2x for simplicity of using this
      // field to make guest-related decisions. Render pass objects are not very
      // expensive, and their dependencies can't be shared between 2x-as-4x and
      // true 4x MSAA passes (framebuffers because render target cache render
      // targets are different for 2x and 4x guest MSAA, pipelines because the
      // sample mask will have 2 samples excluded for 2x-as-4x).
      // This has effect only on the attachments, but even in cases when there
      // are no attachments, it can be used to pass the sample count between
      // subsystems, for instance, to specify the desired number of samples to
      // use when there are no attachments in pipelines.
      // Also, without attachments, using separate render passes for different
      // sample counts ensures that if the variableMultisampleRate feature is
      // not supported, no draws with different rasterization sample counts end
      // up in one render pass.
      xenos::MsaaSamples msaa_samples : xenos::kMsaaSamplesBits;  // 2
      // << 0 is depth, << 1...4 is color.
      uint32_t depth_and_color_used : 1 + xenos::kMaxColorRenderTargets;  // 7
      // 0 for unused attachments.
      // If VK_FORMAT_D24_UNORM_S8_UINT is not supported, this must be kD24FS8
      // even for kD24S8.
      xenos::DepthRenderTargetFormat depth_format : xenos::kDepthRenderTargetFormatBits;  // 8
      // Linear or sRGB included if host sRGB is used.
      xenos::ColorRenderTargetFormat color_0_view_format
          : xenos::kColorRenderTargetFormatBits;  // 12
      xenos::ColorRenderTargetFormat color_1_view_format
          : xenos::kColorRenderTargetFormatBits;  // 16
      xenos::ColorRenderTargetFormat color_2_view_format
          : xenos::kColorRenderTargetFormatBits;  // 20
      xenos::ColorRenderTargetFormat color_3_view_format
          : xenos::kColorRenderTargetFormatBits;    // 24
      uint32_t color_rts_use_transfer_formats : 1;  // 25
    };
    uint32_t key = 0;
    struct Hasher {
      size_t operator()(const RenderPassKey& key) const { return std::hash<uint32_t>{}(key.key); }
    };
    bool operator==(const RenderPassKey& other_key) const { return key == other_key.key; }
    bool operator!=(const RenderPassKey& other_key) const { return !(*this == other_key); }
    bool operator<(const RenderPassKey& other_key) const { return key < other_key.key; }
  };
  static_assert_size(RenderPassKey, sizeof(uint32_t));

  struct Framebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkExtent2D host_extent{};
    Framebuffer() = default;
    Framebuffer(VkFramebuffer framebuffer, const VkExtent2D& host_extent)
        : framebuffer(framebuffer), host_extent(host_extent) {}
  };

  VulkanRenderTargetCache(const RegisterFile& register_file, const memory::Memory& memory,
                          TraceWriter& trace_writer, uint32_t draw_resolution_scale_x,
                          uint32_t draw_resolution_scale_y,
                          VulkanCommandProcessor& command_processor);
  ~VulkanRenderTargetCache();

  // Transient descriptor set layouts must be initialized in the command
  // processor.
  bool Initialize(uint32_t shared_memory_binding_count);
  void Shutdown(bool from_destructor = false);
  void ClearCache() override;

  // The render targets bound by the last Update are original resolution ones
  // (IsOriginalResolutionRenderTarget), drawn at the guest resolution.
  bool IsLastUpdateOriginalResolution() const {
    RenderTarget* const* render_targets = last_update_accumulated_render_targets();
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      if (render_targets[i] && render_targets[i]->key().original_resolution) {
        return true;
      }
    }
    return false;
  }

  void CompletedSubmissionUpdated();
  void EndSubmission();

  // Packed output of a native resolve, when the whole
  // requested texture range is still valid. Records its submission lifetime
  // and pushes the barrier for a texture load compute shader. GPU rewrites
  // of matching buffers are ordered after all their earlier consumers.
  bool UseNativeResolveBufferRange(uint32_t address, uint32_t length,
                                   VkDescriptorBufferInfo& buffer_info, bool scaled = false);
  bool CanUseNativeResolveBufferRange(uint32_t address, uint32_t length, bool scaled);
  // Whether a texture source not inside any one native resolve buffer can be
  // assembled from those overlapping it and guest memory (with no other
  // GPU-written data in it), and assembling it in a temporary buffer for the
  // current submission.
  bool CanComposeNativeResolveRange(uint32_t address, uint32_t length, bool scaled,
                                    SharedMemory& shared_memory);
  bool ComposeNativeResolveRange(uint32_t address, uint32_t length, bool scaled,
                                 SharedMemory& shared_memory, VkDescriptorBufferInfo& buffer_info);
  // Diagnostics: the native resolve buffers overlapping the range.
  std::string DescribeNativeResolveBuffers(uint32_t address, uint32_t length, bool scaled);
  bool FlushNativeResolveMemory(uint32_t address, uint32_t length, bool scaled);

  Path GetPath() const override { return path_; }

  // True when render_target_path_vulkan == "native": conventional host render
  // targets, with native resolves and overwrite proofs. Data needed after a
  // surface rebind is preserved; the shadow map shares the main depth storage.
  bool native_rt_mode() const { return native_rt_mode_; }
  bool native_resolve_enabled() const { return native_resolve_enabled_; }

  // native_resolve_scaled_lazy_memory: color native resolves with draw
  // resolution scaling leave the scaled resolve memory unwritten while their
  // data is in the first target texture; these write it back when something is
  // about to read that memory (unscaled range), or everything (before traces
  // and cache clears).
  void FlushPendingScaledResolveMemory(uint32_t start, uint32_t length);
  void FlushAllPendingScaledResolveMemory();

  // Ownership-transfer draws elided since startup by the native mode - the
  // concrete measure of what this mode removes from the frame.
  uint64_t transfers_skipped() const { return transfers_skipped_; }

  VkBuffer edram_buffer() const { return edram_buffer_; }

  // Performs the resolve to a shared memory area according to the current
  // register values, and also clears the render targets if needed. Must be in a
  // frame for calling.
  bool Resolve(const memory::Memory& memory, VulkanSharedMemory& shared_memory,
               VulkanTextureCache& texture_cache, uint32_t& written_address_out,
               uint32_t& written_length_out);

  // Returns true if any downloads were submitted to the command processor.
  bool InitializeTraceSubmitDownloads();
  void InitializeTraceCompleteDownloads();
  void RestoreEdramSnapshot(const void* snapshot);

  bool Update(bool is_rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
              uint32_t normalized_color_mask, const Shader& vertex_shader) override;
  // Binding information for the last successful update.
  // native_resolve_copy_free: a native resolve of a whole render target into a
  // texture that can take over its image is held back until the next draw. If
  // that draw overwrites the whole render target (and only it), the render
  // target and the texture exchange images instead of copying; otherwise, or
  // before anything else uses them (vertex_shader nullptr), the copy is done.
  // Before the draw's textures are requested.
  void ProcessPendingCopyFreeResolve(const Shader* vertex_shader, bool is_rasterization_done,
                                     reg::RB_DEPTHCONTROL normalized_depth_control,
                                     uint32_t normalized_color_mask);
  void FlushPendingCopyFreeResolve() {
    ProcessPendingCopyFreeResolve(nullptr, false, reg::RB_DEPTHCONTROL(), 0);
  }

  // Native renderer (native_rt_clear_draws_as_clears): a draw of the XDK
  // clear shaders that replaces everything it writes with constant values in
  // a rectangle, done as a clear of the attachments there. Returns true if
  // done, and the draw must then not be issued. After Update.
  bool ClearDrawAsAttachmentClear(reg::RB_DEPTHCONTROL normalized_depth_control,
                                  uint32_t normalized_color_mask, const Shader& vertex_shader,
                                  const Shader* pixel_shader);

  RenderPassKey last_update_render_pass_key() const { return last_update_render_pass_key_; }
  VkRenderPass last_update_render_pass() const { return last_update_render_pass_; }
  const Framebuffer* last_update_framebuffer() const { return last_update_framebuffer_; }
  void GetLastUpdateRenderingAttachments(VkRenderingAttachmentInfo* color_attachments,
                                         uint32_t* color_attachment_count_out,
                                         VkRenderingAttachmentInfo* depth_attachment,
                                         VkRenderingAttachmentInfo* stencil_attachment) const;

  // Using R16G16[B16A16]_SNORM, which are -1...1, not the needed -32...32.
  // Persistent data doesn't depend on this, so can be overriden by per-game
  // configuration.
  bool IsFixedRG16TruncatedToMinus1To1() const {
    return GetPath() == Path::kHostRenderTargets && !color_rg16_draw_format_fallback_to_float_ &&
           !REXCVAR_GET(snorm16_render_target_full_range);
  }
  bool IsFixedRGBA16TruncatedToMinus1To1() const {
    return GetPath() == Path::kHostRenderTargets && !color_rgba16_draw_format_fallback_to_float_ &&
           !REXCVAR_GET(snorm16_render_target_full_range);
  }
  bool gamma_render_target_as_unorm16() const { return gamma_render_target_as_unorm16_; }

  bool depth_unorm24_vulkan_format_supported() const {
    return depth_unorm24_vulkan_format_supported_;
  }
  bool depth_float24_round() const { return depth_float24_round_; }
  bool depth_float24_convert_in_pixel_shader() const {
    return depth_float24_convert_in_pixel_shader_;
  }

  bool msaa_2x_attachments_supported() const { return msaa_2x_attachments_supported_; }
  bool msaa_2x_no_attachments_supported() const { return msaa_2x_no_attachments_supported_; }
  bool IsMsaa2xSupported(bool subpass_has_attachments) const {
    return subpass_has_attachments ? msaa_2x_attachments_supported_
                                   : msaa_2x_no_attachments_supported_;
  }

  // Returns the render pass object, or VK_NULL_HANDLE if failed to create.
  // A render pass managed by the render target cache may be ended and resumed
  // at any time (to allow for things like copying and texture loading).
  VkRenderPass GetHostRenderTargetsRenderPass(RenderPassKey key);
  VkRenderPass GetFragmentShaderInterlockRenderPass() const {
    assert_true(GetPath() == Path::kPixelShaderInterlock);
    return fsi_render_pass_;
  }

  VkFormat GetDepthVulkanFormat(xenos::DepthRenderTargetFormat format) const;
  VkFormat GetColorVulkanFormat(xenos::ColorRenderTargetFormat format) const;
  VkFormat GetColorOwnershipTransferVulkanFormat(
      xenos::ColorRenderTargetFormat format,
      xenos::MsaaSamples msaa_samples = xenos::MsaaSamples::k1X,
      bool* is_integer_out = nullptr) const;

 protected:
  uint32_t GetMaxRenderTargetWidth() const override;
  uint32_t GetMaxRenderTargetHeight() const override;

  RenderTarget* CreateRenderTarget(RenderTargetKey key) override;
  void EnsureRenderTargetTileRows(RenderTargetKey key, uint32_t tile_rows) override;

  bool IsHostDepthEncodingDifferent(xenos::DepthRenderTargetFormat format) const override;

  bool IsGammaFormatHostStorageSeparate() const override;

  void RequestPixelShaderInterlockBarrier() override;

 private:
  enum class EdramBufferUsage {
    // There's no need for combined fragment and compute usages.
    // With host render targets, the usual usage sequence is as follows:
    // - Optionally compute writes - host depth copy storing for EDRAM range
    //   ownership transfers.
    // - Optionally fragment reads - host depth copy storing for EDRAM range
    //   ownership transfers.
    // - Compute writes - copying from host render targets during resolving.
    // - Compute reads - writing to the shared memory during resolving.
    // With the render backend implementation based on fragment shader
    // interlocks, it's:
    // - Fragment reads and writes - depth / stencil and color operations.
    // - Compute reads - writing to the shared memory during resolving.
    // So, fragment reads and compute reads normally don't follow each other,
    // and there's no need to amortize the cost of a read > read barrier in an
    // exceptional situation by using a wider barrier in the normal scenario.

    // Host depth copy storing.
    kFragmentRead,
    // Fragment shader interlock depth / stencil and color operations.
    kFragmentReadWrite,
    // Resolve - copying to the shared memory.
    kComputeRead,
    // Resolve - copying from host render targets.
    kComputeWrite,
    // Trace recording.
    kTransferRead,
    // Trace playback.
    kTransferWrite,
  };

  enum class EdramBufferModificationStatus {
    // The values are ordered by how strong the barrier conditions are.
    // No uncommitted shader writes.
    kUnmodified,
    // Need to commit before the next fragment shader interlock usage with
    // overlap.
    kViaFragmentShaderInterlock,
    // Need to commit before any next fragment shader interlock usage.
    kViaUnordered,
  };

  enum ResolveCopyDescriptorSet : uint32_t {
    // Never changes.
    kResolveCopyDescriptorSetEdram,
    // Shared memory or a region in it.
    kResolveCopyDescriptorSetDest,

    kResolveCopyDescriptorSetCount,
  };

  struct ResolveCopyShaderCode {
    const uint32_t* unscaled;
    size_t unscaled_size_bytes;
    const uint32_t* scaled;
    size_t scaled_size_bytes;
  };

  static void GetEdramBufferUsageMasks(EdramBufferUsage usage, VkPipelineStageFlags& stage_mask_out,
                                       VkAccessFlags& access_mask_out);
  bool IsColor16FormatFloatLike(xenos::ColorRenderTargetFormat format) const;
  void UseEdramBuffer(EdramBufferUsage new_usage);
  void MarkEdramBufferModified(EdramBufferModificationStatus modification_status =
                                   EdramBufferModificationStatus::kViaUnordered);
  void CommitEdramBufferShaderWrites(
      EdramBufferModificationStatus commit_status =
          EdramBufferModificationStatus::kViaFragmentShaderInterlock);

  VulkanCommandProcessor& command_processor_;
  const memory::Memory& memory_;
  TraceWriter& trace_writer_;

  Path path_ = Path::kHostRenderTargets;
  bool native_rt_mode_ = false;
  uint64_t transfers_skipped_ = 0;

  // Accessible in fragment and compute shaders.
  VkDescriptorSetLayout descriptor_set_layout_storage_buffer_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_sampled_image_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_sampled_image_x2_ = VK_NULL_HANDLE;

  std::unique_ptr<ui::vulkan::SingleLayoutDescriptorSetPool> descriptor_set_pool_sampled_image_;
  std::unique_ptr<ui::vulkan::SingleLayoutDescriptorSetPool> descriptor_set_pool_sampled_image_x2_;

  VkDeviceMemory edram_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer edram_buffer_ = VK_NULL_HANDLE;
  EdramBufferUsage edram_buffer_usage_;
  // HAND PATCH: freshly allocated device-local memory contains garbage;
  // games (and the FSI path especially) can read EDRAM regions before ever
  // writing them, showing green static. Zero the buffer on first use.
  bool edram_buffer_initial_cleared_ = false;
  EdramBufferModificationStatus edram_buffer_modification_status_ =
      EdramBufferModificationStatus::kUnmodified;
  VkDescriptorPool edram_storage_buffer_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet edram_storage_buffer_descriptor_set_;

  VkPipelineLayout resolve_copy_pipeline_layout_ = VK_NULL_HANDLE;
  static const ResolveCopyShaderCode
      kResolveCopyShaders[size_t(draw_util::ResolveCopyShaderIndex::kCount)];
  std::array<VkPipeline, size_t(draw_util::ResolveCopyShaderIndex::kCount)>
      resolve_copy_pipelines_{};

  // On the fragment shader interlock path, the render pass key is used purely
  // for passing parameters to pipeline setup - there's always only one render
  // pass.
  RenderPassKey last_update_render_pass_key_;
  VkRenderPass last_update_render_pass_ = VK_NULL_HANDLE;
  // The pitch is not used on the fragment shader interlock path.
  uint32_t last_update_framebuffer_pitch_tiles_at_32bpp_ = 0;
  // The attachments are not used on the fragment shader interlock path.
  const RenderTarget* const*
      last_update_framebuffer_attachments_[1 + xenos::kMaxColorRenderTargets] = {};
  const Framebuffer* last_update_framebuffer_ = VK_NULL_HANDLE;

  // For host render targets.

  // Can only be destroyed when framebuffers referencing it are destroyed!
  class VulkanRenderTarget final : public RenderTarget {
   public:
    static constexpr VkPipelineStageFlags kColorDrawStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    static constexpr VkAccessFlags kColorDrawAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    static constexpr VkImageLayout kColorDrawLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    static constexpr VkPipelineStageFlags kDepthDrawStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    static constexpr VkAccessFlags kDepthDrawAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    static constexpr VkImageLayout kDepthDrawLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // The image of a render target and the objects referring to it, replaced
    // together when the image grows (native_rt_size_by_use).
    struct Image {
      VkImage image = VK_NULL_HANDLE;
      VkDeviceMemory memory = VK_NULL_HANDLE;
      VkImageView view_depth_color = VK_NULL_HANDLE;
      VkImageView view_depth_stencil = VK_NULL_HANDLE;
      VkImageView view_stencil = VK_NULL_HANDLE;
      VkImageView view_srgb = VK_NULL_HANDLE;
      VkImageView view_color_transfer_separate = VK_NULL_HANDLE;
      size_t descriptor_set_index_transfer_source = SIZE_MAX;
      // Rows of EDRAM tiles (of the render target's pitch) the image covers.
      uint32_t tile_rows = 0;
    };

    // Takes ownership of the Vulkan objects passed to the constructor.
    VulkanRenderTarget(RenderTargetKey key, VulkanRenderTargetCache& render_target_cache,
                       const Image& image)
        : RenderTarget(key),
          render_target_cache_(render_target_cache),
          image_(image.image),
          memory_(image.memory),
          view_depth_color_(image.view_depth_color),
          view_depth_stencil_(image.view_depth_stencil),
          view_stencil_(image.view_stencil),
          view_srgb_(image.view_srgb),
          view_color_transfer_separate_(image.view_color_transfer_separate),
          descriptor_set_index_transfer_source_(image.descriptor_set_index_transfer_source),
          tile_rows_(image.tile_rows) {}
    ~VulkanRenderTarget();

    uint32_t tile_rows() const { return tile_rows_; }
    // native_rt_debug_regrow_interval.
    uint32_t& debug_regrow_counter() { return debug_regrow_counter_; }
    // Takes ownership of the new image's objects, returns the old ones (the
    // caller destroys them once the GPU is done with them). The usage state is
    // the caller's to update.
    Image ReplaceImage(const Image& image) {
      Image old_image;
      old_image.image = image_;
      old_image.memory = memory_;
      old_image.view_depth_color = view_depth_color_;
      old_image.view_depth_stencil = view_depth_stencil_;
      old_image.view_stencil = view_stencil_;
      old_image.view_srgb = view_srgb_;
      old_image.view_color_transfer_separate = view_color_transfer_separate_;
      old_image.descriptor_set_index_transfer_source = descriptor_set_index_transfer_source_;
      old_image.tile_rows = tile_rows_;
      image_ = image.image;
      memory_ = image.memory;
      view_depth_color_ = image.view_depth_color;
      view_depth_stencil_ = image.view_depth_stencil;
      view_stencil_ = image.view_stencil;
      view_srgb_ = image.view_srgb;
      view_color_transfer_separate_ = image.view_color_transfer_separate;
      descriptor_set_index_transfer_source_ = image.descriptor_set_index_transfer_source;
      tile_rows_ = image.tile_rows;
      return old_image;
    }

    VkImage image() const { return image_; }
    VkDeviceMemory memory() const { return memory_; }

    VkImageView view_depth_color() const { return view_depth_color_; }
    VkImageView view_depth_stencil() const { return view_depth_stencil_; }
    VkImageView view_color_transfer_separate() const { return view_color_transfer_separate_; }
    VkImageView view_color_transfer() const {
      return view_color_transfer_separate_ != VK_NULL_HANDLE ? view_color_transfer_separate_
                                                             : view_depth_color_;
    }
    VkDescriptorSet GetDescriptorSetTransferSource() const {
      ui::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
          key().is_depth ? *render_target_cache_.descriptor_set_pool_sampled_image_x2_
                         : *render_target_cache_.descriptor_set_pool_sampled_image_;
      return descriptor_set_pool.Get(descriptor_set_index_transfer_source_);
    }

    static void GetDrawUsage(bool is_depth, VkPipelineStageFlags* stage_mask_out,
                             VkAccessFlags* access_mask_out, VkImageLayout* layout_out) {
      if (stage_mask_out) {
        *stage_mask_out = is_depth ? kDepthDrawStageMask : kColorDrawStageMask;
      }
      if (access_mask_out) {
        *access_mask_out = is_depth ? kDepthDrawAccessMask : kColorDrawAccessMask;
      }
      if (layout_out) {
        *layout_out = is_depth ? kDepthDrawLayout : kColorDrawLayout;
      }
    }
    void GetDrawUsage(VkPipelineStageFlags* stage_mask_out, VkAccessFlags* access_mask_out,
                      VkImageLayout* layout_out) const {
      GetDrawUsage(key().is_depth, stage_mask_out, access_mask_out, layout_out);
    }
    VkPipelineStageFlags current_stage_mask() const { return current_stage_mask_; }
    VkAccessFlags current_access_mask() const { return current_access_mask_; }
    VkImageLayout current_layout() const { return current_layout_; }
    void SetUsage(VkPipelineStageFlags stage_mask, VkAccessFlags access_mask,
                  VkImageLayout layout) {
      current_stage_mask_ = stage_mask;
      current_access_mask_ = access_mask;
      current_layout_ = layout;
    }

    uint32_t temporary_sort_index() const { return temporary_sort_index_; }
    void SetTemporarySortIndex(uint32_t index) { temporary_sort_index_ = index; }

    // Depth: a rectangle (guest pixels) where the stencil is known to have one
    // value - cleared by a draw, not written since. Valid while the generation
    // is the render target cache's uniform_stencil_generation_.
    struct UniformStencil {
      Transfer::Rectangle rectangle = {};
      uint32_t value = 0;
      uint64_t generation = 0;
    };
    UniformStencil& uniform_stencil() { return uniform_stencil_; }

   private:
    VulkanRenderTargetCache& render_target_cache_;

    VkImage image_;
    VkDeviceMemory memory_;

    // TODO(Triang3l): Per-format drawing views for mutable formats with EDRAM
    // aliasing without transfers.
    VkImageView view_depth_color_;
    // Optional views.
    VkImageView view_depth_stencil_;
    VkImageView view_stencil_;
    VkImageView view_srgb_;
    VkImageView view_color_transfer_separate_;

    // 2 sampled images for depth / stencil, 1 sampled image for color.
    size_t descriptor_set_index_transfer_source_;

    uint32_t tile_rows_;
    uint32_t debug_regrow_counter_ = 0;

    VkPipelineStageFlags current_stage_mask_ = 0;
    VkAccessFlags current_access_mask_ = 0;
    VkImageLayout current_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    // Temporary storage for indices in operations like transfers and dumps.
    uint32_t temporary_sort_index_ = 0;

    UniformStencil uniform_stencil_;
  };

  struct FramebufferKey {
    RenderPassKey render_pass_key;

    // Same as RenderTargetKey::pitch_tiles_at_32bpp.
    uint32_t pitch_tiles_at_32bpp : 8;  // 8
    // [0, 2047].
    uint32_t depth_base_tiles : xenos::kEdramBaseTilesBits;    // 19
    uint32_t color_0_base_tiles : xenos::kEdramBaseTilesBits;  // 30

    uint32_t color_1_base_tiles : xenos::kEdramBaseTilesBits;  // 43
    uint32_t color_2_base_tiles : xenos::kEdramBaseTilesBits;  // 54

    uint32_t color_3_base_tiles : xenos::kEdramBaseTilesBits;  // 75
    // The attachments are original resolution render targets.
    uint32_t original_resolution : 1;  // 76

    // Including all the padding, for a stable hash.
    FramebufferKey() { Reset(); }
    FramebufferKey(const FramebufferKey& key) { std::memcpy(this, &key, sizeof(*this)); }
    FramebufferKey& operator=(const FramebufferKey& key) {
      std::memcpy(this, &key, sizeof(*this));
      return *this;
    }
    bool operator==(const FramebufferKey& key) const {
      return std::memcmp(this, &key, sizeof(*this)) == 0;
    }
    using Hasher = rex::XXHasher<FramebufferKey>;
    void Reset() { std::memset(this, 0, sizeof(*this)); }
  };

  enum TransferUsedDescriptorSet : uint32_t {
    // Ordered from the least to the most frequently changed.
    kTransferUsedDescriptorSetHostDepthBuffer,
    kTransferUsedDescriptorSetHostDepthStencilTextures,
    kTransferUsedDescriptorSetDepthStencilTextures,
    // Mutually exclusive with kTransferUsedDescriptorSetDepthStencilTextures.
    kTransferUsedDescriptorSetColorTexture,

    kTransferUsedDescriptorSetCount,

    kTransferUsedDescriptorSetHostDepthBufferBit = uint32_t(1)
                                                   << kTransferUsedDescriptorSetHostDepthBuffer,
    kTransferUsedDescriptorSetHostDepthStencilTexturesBit =
        uint32_t(1) << kTransferUsedDescriptorSetHostDepthStencilTextures,
    kTransferUsedDescriptorSetDepthStencilTexturesBit =
        uint32_t(1) << kTransferUsedDescriptorSetDepthStencilTextures,
    kTransferUsedDescriptorSetColorTextureBit = uint32_t(1)
                                                << kTransferUsedDescriptorSetColorTexture,
  };

  // 32-bit push constants (for simplicity of size calculation and to avoid
  // std140 packing issues).
  enum TransferUsedPushConstantDword : uint32_t {
    kTransferUsedPushConstantDwordHostDepthAddress,
    kTransferUsedPushConstantDwordAddress,
    // Changed 8 times per transfer.
    kTransferUsedPushConstantDwordStencilMask,

    kTransferUsedPushConstantDwordCount,

    kTransferUsedPushConstantDwordHostDepthAddressBit =
        uint32_t(1) << kTransferUsedPushConstantDwordHostDepthAddress,
    kTransferUsedPushConstantDwordAddressBit = uint32_t(1) << kTransferUsedPushConstantDwordAddress,
    kTransferUsedPushConstantDwordStencilMaskBit = uint32_t(1)
                                                   << kTransferUsedPushConstantDwordStencilMask,
  };

  enum class TransferPipelineLayoutIndex {
    kColor,
    kDepth,
    kColorToStencilBit,
    kDepthToStencilBit,
    kColorAndHostDepthTexture,
    kColorAndHostDepthBuffer,
    kDepthAndHostDepthTexture,
    kDepthAndHostDepthBuffer,

    kCount,
  };

  struct TransferPipelineLayoutInfo {
    uint32_t used_descriptor_sets;
    uint32_t used_push_constant_dwords;
  };

  static const TransferPipelineLayoutInfo
      kTransferPipelineLayoutInfos[size_t(TransferPipelineLayoutIndex::kCount)];

  enum class TransferMode : uint32_t {
    kColorToDepth,
    kColorToColor,

    kDepthToDepth,
    kDepthToColor,

    kColorToStencilBit,
    kDepthToStencilBit,

    // Two-source modes, using the host depth if it, when converted to the guest
    // format, matches what's in the owner source (not modified, keep host
    // precision), or the guest data otherwise (significantly modified, possibly
    // cleared). Stencil for FragStencilRef is always taken from the guest
    // source.

    kColorAndHostDepthToDepth,
    // When using different source and destination depth formats.
    kDepthAndHostDepthToDepth,

    // If host depth is fetched, but it's the same image as the destination,
    // it's copied to the EDRAM buffer (but since it's just a scratch buffer,
    // with tiles laid out linearly with the same pitch as in the original
    // render target; also no swapping of 40-sample columns as opposed to the
    // host render target - this is done only for the color source) and fetched
    // from there instead of the host depth texture.
    kColorAndHostDepthCopyToDepth,
    kDepthAndHostDepthCopyToDepth,

    kCount,
  };

  enum class TransferOutput {
    kColor,
    kDepth,
    kStencilBit,
  };

  struct TransferModeInfo {
    TransferOutput output;
    TransferPipelineLayoutIndex pipeline_layout;
  };

  static const TransferModeInfo kTransferModes[size_t(TransferMode::kCount)];

  union TransferShaderKey {
    uint32_t key;
    struct {
      xenos::MsaaSamples dest_msaa_samples : xenos::kMsaaSamplesBits;
      uint32_t dest_color_rt_index : xenos::kColorRenderTargetIndexBits;
      uint32_t dest_resource_format : xenos::kRenderTargetFormatBits;
      xenos::MsaaSamples source_msaa_samples : xenos::kMsaaSamplesBits;
      // Always 1x when the host depth is a copy from a buffer rather than an
      // image, not to create the same pipeline for different MSAA sample counts
      // as it doesn't matter in this case.
      xenos::MsaaSamples host_depth_source_msaa_samples : xenos::kMsaaSamplesBits;
      uint32_t source_resource_format : xenos::kRenderTargetFormatBits;

      // Last bits because this affects the pipeline layout - after sorting,
      // only change it as fewer times as possible. Depth buffers have an
      // additional stencil texture.
      static_assert(size_t(TransferMode::kCount) <= (size_t(1) << 4));
      TransferMode mode : 4;
    };

    TransferShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const TransferShaderKey& key) const {
        return std::hash<uint32_t>{}(key.key);
      }
    };
    bool operator==(const TransferShaderKey& other_key) const { return key == other_key.key; }
    bool operator!=(const TransferShaderKey& other_key) const { return !(*this == other_key); }
    bool operator<(const TransferShaderKey& other_key) const { return key < other_key.key; }
  };

  struct TransferPipelineKey {
    RenderPassKey render_pass_key;
    TransferShaderKey shader_key;

    TransferPipelineKey(RenderPassKey render_pass_key, TransferShaderKey shader_key)
        : render_pass_key(render_pass_key), shader_key(shader_key) {}

    struct Hasher {
      size_t operator()(const TransferPipelineKey& key) const {
        XXH3_state_t hash_state;
        XXH3_64bits_reset(&hash_state);
        XXH3_64bits_update(&hash_state, &key.render_pass_key, sizeof(key.render_pass_key));
        XXH3_64bits_update(&hash_state, &key.shader_key, sizeof(key.shader_key));
        return static_cast<size_t>(XXH3_64bits_digest(&hash_state));
      }
    };
    bool operator==(const TransferPipelineKey& other_key) const {
      return render_pass_key == other_key.render_pass_key && shader_key == other_key.shader_key;
    }
    bool operator!=(const TransferPipelineKey& other_key) const { return !(*this == other_key); }
    bool operator<(const TransferPipelineKey& other_key) const {
      if (render_pass_key != other_key.render_pass_key) {
        return render_pass_key < other_key.render_pass_key;
      }
      return shader_key < other_key.shader_key;
    }
  };

  union TransferAddressConstant {
    uint32_t constant;
    struct {
      // All in tiles.
      uint32_t dest_pitch : xenos::kEdramPitchTilesBits;
      uint32_t source_pitch : xenos::kEdramPitchTilesBits;
      // Destination base in tiles minus source base in tiles (not vice versa
      // because this is a transform of the coordinate system, not addresses
      // themselves).
      // + 1 bit because this is a signed difference between two EDRAM bases.
      // 0 for host_depth_source_is_copy (ignored in this case anyway as
      // destination == source anyway).
      int32_t source_to_dest : xenos::kEdramBaseTilesBits + 1;
    };
    TransferAddressConstant() : constant(0) { static_assert_size(*this, sizeof(constant)); }
    bool operator==(const TransferAddressConstant& other_constant) const {
      return constant == other_constant.constant;
    }
    bool operator!=(const TransferAddressConstant& other_constant) const {
      return !(*this == other_constant);
    }
  };

  struct TransferInvocation {
    Transfer transfer;
    TransferShaderKey shader_key;
    TransferInvocation(const Transfer& transfer, const TransferShaderKey& shader_key)
        : transfer(transfer), shader_key(shader_key) {}
    bool operator<(const TransferInvocation& other_invocation) const {
      // TODO(Triang3l): See if it may be better to sort by the source in the
      // first place, especially when reading the same data multiple times (like
      // to write the stencil bits after depth) for better read locality.
      // Sort by the shader key primarily to reduce pipeline state (context)
      // switches.
      if (shader_key != other_invocation.shader_key) {
        return shader_key < other_invocation.shader_key;
      }
      // Host depth render targets are changed rarely if they exist, won't save
      // many binding changes, ignore them for simplicity (their existence is
      // caught by the shader key change).
      assert_not_null(transfer.source);
      assert_not_null(other_invocation.transfer.source);
      uint32_t source_index =
          static_cast<const VulkanRenderTarget*>(transfer.source)->temporary_sort_index();
      uint32_t other_source_index =
          static_cast<const VulkanRenderTarget*>(other_invocation.transfer.source)
              ->temporary_sort_index();
      if (source_index != other_source_index) {
        return source_index < other_source_index;
      }
      return transfer.start_tiles < other_invocation.transfer.start_tiles;
    }
    bool CanBeMergedIntoOneDraw(const TransferInvocation& other_invocation) const {
      return shader_key == other_invocation.shader_key &&
             transfer.AreSourcesSame(other_invocation.transfer);
    }
  };

  union DumpPipelineKey {
    uint32_t key;
    struct {
      xenos::MsaaSamples msaa_samples : 2;
      uint32_t resource_format : 4;
      // Last bit because this affects the pipeline - after sorting, only change
      // it at most once. Depth buffers have an additional stencil SRV.
      uint32_t is_depth : 1;
    };

    DumpPipelineKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const DumpPipelineKey& key) const { return std::hash<uint32_t>{}(key.key); }
    };
    bool operator==(const DumpPipelineKey& other_key) const { return key == other_key.key; }
    bool operator!=(const DumpPipelineKey& other_key) const { return !(*this == other_key); }
    bool operator<(const DumpPipelineKey& other_key) const { return key < other_key.key; }

    xenos::ColorRenderTargetFormat GetColorFormat() const {
      assert_false(is_depth);
      return xenos::ColorRenderTargetFormat(resource_format);
    }
    xenos::DepthRenderTargetFormat GetDepthFormat() const {
      assert_true(is_depth);
      return xenos::DepthRenderTargetFormat(resource_format);
    }
  };

  // There's no strict dependency on the group size in dumping, for simplicity
  // calculations especially with resolution scaling, dividing manually (as the
  // group size is not unlimited). The only restriction is that an integer
  // multiple of it must be 80x16 samples (and no larger than that) for 32bpp,
  // or 40x16 samples for 64bpp (because only a half of the pair of tiles may
  // need to be dumped). Using 8x16 since that's 128 - the minimum required
  // group size on Vulkan, and the maximum number of lanes in a subgroup on
  // Vulkan.
  static constexpr uint32_t kDumpSamplesPerGroupX = 8;
  static constexpr uint32_t kDumpSamplesPerGroupY = 16;

  union DumpPitches {
    uint32_t pitches;
    struct {
      // Both in tiles.
      uint32_t dest_pitch : xenos::kEdramPitchTilesBits;
      uint32_t source_pitch : xenos::kEdramPitchTilesBits;
    };
    DumpPitches() : pitches(0) { static_assert_size(*this, sizeof(pitches)); }
    bool operator==(const DumpPitches& other_pitches) const {
      return pitches == other_pitches.pitches;
    }
    bool operator!=(const DumpPitches& other_pitches) const { return !(*this == other_pitches); }
  };

  union DumpOffsets {
    uint32_t offsets;
    struct {
      // May be beyond the EDRAM tile count in case of EDRAM addressing
      // wrapping, thus + 1 bit.
      uint32_t dispatch_first_tile : xenos::kEdramBaseTilesBits + 1;
      uint32_t source_base_tiles : xenos::kEdramBaseTilesBits;
    };
    DumpOffsets() : offsets(0) { static_assert_size(*this, sizeof(offsets)); }
    bool operator==(const DumpOffsets& other_offsets) const {
      return offsets == other_offsets.offsets;
    }
    bool operator!=(const DumpOffsets& other_offsets) const { return !(*this == other_offsets); }
  };

  enum DumpDescriptorSet : uint32_t {
    // Never changes. Same in both color and depth pipeline layouts, keep the
    // first for pipeline layout compatibility, to only have to set it once.
    kDumpDescriptorSetEdram,
    // One resolve may need multiple sources. Different descriptor set layouts
    // for color and depth.
    kDumpDescriptorSetSource,

    kDumpDescriptorSetCount,
  };

  enum DumpPushConstant : uint32_t {
    // May be different for different sources.
    kDumpPushConstantPitches,
    // May be changed multiple times for the same source.
    kDumpPushConstantOffsets,

    kDumpPushConstantCount,
  };

  struct DumpInvocation {
    ResolveCopyDumpRectangle rectangle;
    DumpPipelineKey pipeline_key;
    DumpInvocation(const ResolveCopyDumpRectangle& rectangle, const DumpPipelineKey& pipeline_key)
        : rectangle(rectangle), pipeline_key(pipeline_key) {}
    bool operator<(const DumpInvocation& other_invocation) const {
      // Sort by the pipeline key primarily to reduce pipeline state (context)
      // switches.
      if (pipeline_key != other_invocation.pipeline_key) {
        return pipeline_key < other_invocation.pipeline_key;
      }
      assert_not_null(rectangle.render_target);
      uint32_t render_target_index =
          static_cast<const VulkanRenderTarget*>(rectangle.render_target)->temporary_sort_index();
      const ResolveCopyDumpRectangle& other_rectangle = other_invocation.rectangle;
      uint32_t other_render_target_index =
          static_cast<const VulkanRenderTarget*>(other_rectangle.render_target)
              ->temporary_sort_index();
      if (render_target_index != other_render_target_index) {
        return render_target_index < other_render_target_index;
      }
      if (rectangle.row_first != other_rectangle.row_first) {
        return rectangle.row_first < other_rectangle.row_first;
      }
      return rectangle.row_first_start < other_rectangle.row_first_start;
    }
  };

  struct DirectResolvePushConstants {
    draw_util::ResolveCopyShaderConstants resolve;
    uint32_t source_base_tiles;
    uint32_t source_pitch_tiles;
    uint32_t dispatch_first_tile;
  };

  struct DirectResolvePipelineKey {
    DumpPipelineKey dump_pipeline_key;
    draw_util::ResolveCopyShaderIndex copy_shader;
    bool draw_resolution_scaled;
    uint64_t packed() const {
      return uint64_t(dump_pipeline_key.key) | (uint64_t(size_t(copy_shader)) << 32) |
             (uint64_t(draw_resolution_scaled ? 1 : 0) << 40);
    }
    struct Hasher {
      size_t operator()(const DirectResolvePipelineKey& key) const {
        return std::hash<uint64_t>{}(key.packed());
      }
    };
    bool operator==(const DirectResolvePipelineKey& other_key) const {
      return packed() == other_key.packed();
    }
  };

  // Returns the framebuffer object, or VK_NULL_HANDLE if failed to create.
  const Framebuffer* GetHostRenderTargetsFramebuffer(
      RenderPassKey render_pass_key, uint32_t pitch_tiles_at_32bpp,
      const RenderTarget* const* depth_and_color_render_targets);

  VkShaderModule GetTransferShader(TransferShaderKey key);
  // With sample-rate shading, returns a pointer to one pipeline. Without
  // sample-rate shading, returns a pointer to as many pipelines as there are
  // samples. If there was a failure to create a pipeline, returns nullptr.
  VkPipeline const* GetTransferPipelines(TransferPipelineKey key);

  // Do ownership transfers for render targets - each render target / vector may
  // be null / empty in case there's nothing to do for them.
  // resolve_clear_rectangle is expected to be provided by
  // PrepareHostRenderTargetsResolveClear which should do all the needed size
  // bound checks.
  bool TryNativeSurfaceCopies(uint32_t render_target_count, RenderTarget* const* render_targets,
                              const std::vector<Transfer>* render_target_transfers,
                              const Transfer::Rectangle* cutout);
  void PerformTransfersAndResolveClears(
      uint32_t render_target_count, RenderTarget* const* render_targets,
      const std::vector<Transfer>* render_target_transfers,
      const uint64_t* render_target_resolve_clear_values = nullptr,
      const Transfer::Rectangle* resolve_clear_rectangle = nullptr);

  VkPipeline GetDumpPipeline(DumpPipelineKey key);
  VkPipeline GetDirectResolvePipeline(DirectResolvePipelineKey key);
  bool TryResolveCopyDirectly(const draw_util::ResolveInfo& resolve_info,
                              draw_util::ResolveCopyShaderIndex copy_shader,
                              bool draw_resolution_scaled);

  // Writes contents of host render targets within rectangles from
  // ResolveInfo::GetCopyEdramTileSpan to edram_buffer_.
  bool DumpRenderTargets(uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
                         uint32_t dump_pitch);

  // Native resolves: the resolved area of a host render target drawn directly
  // into the textures sampling the resolve destination, with the same result as
  // loading the texture from the memory written by the resolve.
  enum class NativeResolveShader : uint32_t {
    kColorFloat,
    kColorUint,
    kDepth,
    // kColorFloat writing the color through a unorm view of the destination's
    // own format (native_resolve_unorm_views), for a source of that format.
    kColorUnorm,
    // kDepth writing through a float view of the destination's own format.
    kDepthFloat,
    kCount,
  };
  static bool IsNativeResolveDepthShader(NativeResolveShader shader) {
    return shader == NativeResolveShader::kDepth || shader == NativeResolveShader::kDepthFloat;
  }
  enum NativeResolveFlags : uint32_t {
    kNativeResolveFlagSwapRedBlue = 1u << 0,
    kNativeResolveFlagDepthFloat24 = 1u << 1,
    kNativeResolveFlagDepthRoundToNearestEven = 1u << 2,
    // Also store the resolved texels to guest memory, as the EDRAM resolve
    // would, instead of dumping the render target and running the resolve.
    kNativeResolveFlagWriteMemory = 1u << 3,
    // xenos::Endian of the memory, bits 4:5.
    kNativeResolveFlagMemoryEndianShift = 4,
    kNativeResolveFlagMemory64bpp = 1u << 6,
    // The memory is the scaled resolve buffer (with draw resolution scaling),
    // bound from the resolve destination's base.
    kNativeResolveFlagMemoryScaled = 1u << 7,
    // Depth, with native_resolve_quad_stencil_capture_: also store the stencil
    // of each 2x2 quad of host pixels of the rectangle, as one dword, to the
    // buffer bound in place of the memory (not with kNativeResolveFlagWriteMemory).
    kNativeResolveFlagStencilCapture = 1u << 8,
    // Keep a texture's existing channel order when a draw follows an image
    // copy. This affects the attachment only, never the resolved memory.
    kNativeResolveFlagTextureSwapRedBlue = 1u << 9,
  };
  // How a source texel is packed into the texel bits, the same way as when
  // dumping the render target to the EDRAM.
  enum class NativeResolvePacking : uint32_t {
    k8888,
    k2101010,
    kFloat16,
    kFloat32,
    kUint16,
    kUint32,
  };
  struct NativeResolveConstants {
    int32_t source_offset_x;
    int32_t source_offset_y;
    uint32_t flags;
    NativeResolvePacking packing;
    // For kNativeResolveFlagWriteMemory: texel (0, 0) and the tiled pitch.
    uint32_t dest_base_dwords;
    uint32_t dest_pitch_texels;
    // Draw resolution scale for kNativeResolveFlagMemoryScaled: x | (y << 8) |
    // (log2 x << 16) | (log2 y << 20) | (both powers of two << 31).
    uint32_t resolution_scale;
    // For kNativeResolveFlagStencilCapture: the host pixel of the rectangle's
    // origin (x | (y << 16), both even) and the quads per row.
    uint32_t stencil_capture_origin;
    uint32_t stencil_capture_pitch_quads;
    // Physical dword represented by the start of a compact output buffer.
    uint32_t memory_base_dwords;
  };
  struct NativeResolvePlan {
    VulkanRenderTarget* source = nullptr;
    NativeResolveShader shader = NativeResolveShader::kColorFloat;
    NativeResolvePacking packing = NativeResolvePacking::k8888;
    uint32_t flags = 0;
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint32_t dest_base = 0;
    uint32_t dest_pitch_texels = 0;
    uint32_t memory_flags = 0;
    // Whether the first target contains the whole resolve rectangle, so
    // drawing it can also write all of the resolved memory.
    bool can_write_memory = false;
    // Nothing samples the destination yet: the only target is a stand-in with
    // no image (unused color attachment), and the draw only writes the memory.
    bool memory_only = false;
    uint32_t target_count = 0;
    VulkanTextureCache::NativeResolveTarget targets[VulkanTextureCache::kMaxNativeResolveTargets];
  };
  bool InitializeNativeResolve(uint32_t shared_memory_binding_count);
  // For a draw of one rectangle (with the XDK clear shader, or any vertex
  // shader the CPU can run): the render targets (bit 0 depth, bits 1-4 color)
  // it overwrites entirely inside the rectangle, so ownership transfers of
  // their old contents there are dead. exact_edges_out (optional) tells
  // whether the rectangle's edges are on pixel boundaries, so the draw writes
  // nothing outside it at any resolution scale or sample count. Only the
  // candidate targets are checked. With depth_without_stencil, the depth
  // counts as overwritten when only the depth is replaced.
  uint32_t GetDrawOverwrittenRenderTargets(reg::RB_DEPTHCONTROL normalized_depth_control,
                                           uint32_t normalized_color_mask,
                                           const Shader& vertex_shader,
                                           Transfer::Rectangle& rectangle_out,
                                           bool* exact_edges_out = nullptr,
                                           uint32_t candidate_targets = UINT32_MAX,
                                           bool depth_without_stencil = false) const;
  // With native_rt_skip_overwritten_transfers, transfers into render targets
  // that the draws right after binding them clear are held back while
  // consecutive clears keep overwriting them, then performed only outside the
  // cleared area - before the first draw that isn't such a clear, a change of
  // the bindings, or a resolve.
  void FlushDeferredTransfers();
  // If the union of the two rectangles is a rectangle, stores it in
  // accumulated and returns true.
  static bool MergeTransferCutout(Transfer::Rectangle& accumulated,
                                  const Transfer::Rectangle& rectangle);
  // Uniform stencil tracking (native_resolve_uniform_stencil): a clear draw
  // makes the stencil in its rectangle the reference value, a draw that may
  // write the stencil anywhere, or a transfer into the rectangle, forgets it.
  // Depth resolves of a rectangle with a uniform stencil then don't need to
  // capture the stencil for writing the memory later.
  void UpdateUniformStencil(VulkanRenderTarget& depth_render_target,
                            reg::RB_DEPTHCONTROL normalized_depth_control,
                            uint32_t normalized_color_mask, const Shader& vertex_shader);
  void ForgetUniformStencilWrittenByTransfers(VulkanRenderTarget& depth_render_target,
                                              const std::vector<Transfer>& transfers,
                                              const Transfer::Rectangle* cutout);
  bool GetUniformStencil(VulkanRenderTarget& depth_render_target, uint32_t x0, uint32_t y0,
                         uint32_t x1, uint32_t y1, uint32_t& value_out);
  bool EnsureNativeResolvePipelineLayouts();
  void ShutdownNativeResolve();
  VkPipeline GetNativeResolvePipeline(NativeResolveShader shader, VkFormat dest_format);
  // Finds the host render target owning the whole resolve area and the textures
  // to write. Must be called before the memory range is marked as resolved.
  // memory_only plans a resolve of only the memory, without any textures.
  bool PrepareNativeResolve(const draw_util::ResolveInfo& resolve_info,
                            VulkanTextureCache& texture_cache, NativeResolvePlan& plan,
                            bool memory_only = false);
  bool TryNativeResolveImageCopy(VulkanTextureCache& texture_cache, const NativeResolvePlan& plan);
  // Records the texture writes (and with write_memory, the guest memory writes
  // in the first target's draw), and marks the textures as up to date - must be
  // called after the memory range has been marked as resolved.
  // With draw resolution scaling, scaled_memory_descriptor_set (set 1 in place
  // of the shared memory) binds the scaled resolve buffer from the
  // destination's base for the memory writes.
  // Without write_memory, stencil_capture_descriptor_set (a depth resolve's
  // stencil capture in place of the shared memory) makes the first target's
  // draw also capture the stencil (kNativeResolveFlagStencilCapture).
  void PerformNativeResolve(VulkanTextureCache& texture_cache, const NativeResolvePlan& plan,
                            bool write_memory,
                            VkDescriptorSet scaled_memory_descriptor_set = VK_NULL_HANDLE,
                            VkDescriptorSet stencil_capture_descriptor_set = VK_NULL_HANDLE,
                            uint32_t memory_base_dwords = 0);
  bool TryNativeResolveBuffer(const draw_util::ResolveInfo& resolve_info,
                              const NativeResolvePlan& plan, VulkanSharedMemory& shared_memory,
                              VulkanTextureCache& texture_cache, bool source_original_resolution);
  struct NativeResolveBuffer;
  // A part of a composed texture source: from a native buffer, or (null) guest
  // memory.
  struct ComposedSource {
    NativeResolveBuffer* buffer;
    uint32_t address;
    uint32_t length;
  };
  bool GetComposedNativeResolveSources(uint32_t address, uint32_t length, bool scaled,
                                       SharedMemory& shared_memory,
                                       std::vector<ComposedSource>& sources_out);
  // Records assembling the range from the sources at the offset of the buffer
  // (the source barriers included, the destination's are the caller's).
  bool RecordComposedNativeResolveRange(const std::vector<ComposedSource>& sources,
                                        uint32_t address, bool scaled,
                                        SharedMemory& shared_memory,
                                        VkBuffer destination_buffer,
                                        VkDeviceSize destination_offset);
  NativeResolveBuffer* AcquireNativeResolveBuffer(VkDeviceSize size);
  NativeResolveBuffer* FindNativeResolveBufferRange(uint32_t address, uint32_t length, bool scaled);
  // Whether a resolve of the range to the base can write into the valid native
  // buffer of a larger resolve (or of several) holding the range (see
  // TryNativeResolveBuffer).
  bool IsInValidNativeResolveBuffer(uint32_t address, uint32_t length, uint32_t base, bool scaled);
  // Whether a native buffer other resolves build on overlaps the range: one
  // still holding data (valid or with memory pending) of another extent, or one
  // shared by resolves of several extents.
  bool OverlapsLiveNativeResolveBuffer(uint32_t address, uint32_t length, bool scaled);
  // Before a resolve writes the range: earlier native resolve results that
  // only partly overlap it are copied to the mirror first, except those of
  // the buffer the resolve writes into, which keeps them itself.
  bool PrepareNativeResolveMemoryForWrite(uint32_t address, uint32_t length,
                                          const NativeResolveBuffer* writing_into = nullptr);
  bool WriteBackNativeResolveBuffer(NativeResolveBuffer& buffer);
  // Records copying guest memory bytes into the buffer (in the current
  // submission, barriers are the caller's).
  bool UploadGuestMemoryToBuffer(uint32_t address, uint32_t length, VkBuffer buffer,
                                 VkDeviceSize offset);
  void ClearNativeResolveBuffers();
  // Uses the same global critical region as the shared-memory write watches.
  rex::thread::global_critical_region native_resolve_buffers_critical_region_;
  std::vector<std::unique_ptr<NativeResolveBuffer>> native_resolve_buffers_;
  // Resolve destinations (bases) that resolves of several extents write - such
  // as a bloom chain downsampling into the same memory. Their results always
  // stay in native buffers, which the others build on.
  std::unordered_set<uint32_t> multi_extent_resolve_bases_;
  uint64_t native_resolve_buffer_bytes_ = 0;
  bool native_resolve_memory_flushing_ = false;

  bool gamma_render_target_as_unorm16_ = false;

  bool depth_unorm24_vulkan_format_supported_ = false;
  bool depth_float24_round_ = false;
  bool depth_float24_convert_in_pixel_shader_ = false;

  bool msaa_2x_attachments_supported_ = false;
  bool msaa_2x_no_attachments_supported_ = false;
  bool color_16bit_transfer_uint_formats_supported_ = true;
  bool color_32bit_transfer_uint_formats_supported_ = true;
  bool color_rg16_draw_format_fallback_to_float_ = false;
  bool color_rgba16_draw_format_fallback_to_float_ = false;

  // VK_NULL_HANDLE if failed to create.
  std::unordered_map<RenderPassKey, VkRenderPass, RenderPassKey::Hasher> render_passes_;

  std::unordered_map<FramebufferKey, Framebuffer, FramebufferKey::Hasher> framebuffers_;

  // native_rt_size_by_use: render target images sized by the rows of EDRAM
  // tiles drawn to, grown (with their contents copied) when more are needed.
  // Rows covering the whole EDRAM addressing period from the base.
  uint32_t GetFullRenderTargetTileRows(RenderTargetKey key) const;
  bool CreateRenderTargetImage(RenderTargetKey key, uint32_t tile_rows,
                               VulkanRenderTarget::Image& image_out);
  // Views and the transfer source descriptor of an existing render target image.
  bool CreateRenderTargetImageViews(RenderTargetKey key, VkImage image, VkFormat format,
                                    VkFormat transfer_format, bool is_srgb_view_needed,
                                    const VkExtent2D& extent, VulkanRenderTarget::Image& image_out);
  bool CreateRenderTargetImageViewsForKey(RenderTargetKey key, VkImage image,
                                          const VkExtent2D& extent,
                                          VulkanRenderTarget::Image& image_out);
  void DestroyRenderTargetImage(bool is_depth, const VulkanRenderTarget::Image& image);
  // Objects replaced while earlier submissions may still use them, destroyed
  // once those complete (UINT64_MAX: all, with the GPU idle).
  struct RetiredRenderTargetImage {
    uint64_t submission;
    bool is_depth;
    VulkanRenderTarget::Image image;
  };
  std::deque<RetiredRenderTargetImage> retired_render_target_images_;
  // native_resolve_copy_free.
  bool CanResolveCopyFree(const NativeResolvePlan& plan, VulkanTextureCache& texture_cache) const;
  bool TryCopyFreeResolveExchange(const NativeResolvePlan& plan, VulkanTextureCache& texture_cache,
                                  const Shader& vertex_shader,
                                  reg::RB_DEPTHCONTROL normalized_depth_control,
                                  uint32_t normalized_color_mask);
  void RetireFramebuffersOfRenderTarget(RenderTargetKey key);
  struct PendingCopyFreeResolve {
    bool active = false;
    NativeResolvePlan plan;
    VulkanTextureCache* texture_cache = nullptr;
  };
  PendingCopyFreeResolve pending_copy_free_resolve_;
  uint64_t copy_free_resolve_count_ = 0;
  std::deque<std::pair<uint64_t, VkFramebuffer>> retired_framebuffers_;
  void DestroyRetiredRenderTargetObjects(uint64_t completed_submission);

  // Set 0 - EDRAM storage buffer, set 1 - source depth sampled image (and
  // unused stencil from the transfer descriptor set), HostDepthStoreConstants
  // passed via push constants.
  VkPipelineLayout host_depth_store_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X) + 1] = {};

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> transfer_vertex_buffer_pool_;
  VkShaderModule transfer_passthrough_vertex_shader_ = VK_NULL_HANDLE;
  VkPipelineLayout transfer_pipeline_layouts_[size_t(TransferPipelineLayoutIndex::kCount)] = {};
  // VK_NULL_HANDLE if failed to create.
  std::unordered_map<TransferShaderKey, VkShaderModule, TransferShaderKey::Hasher>
      transfer_shaders_;
  // With sample-rate shading, one pipeline per entry. Without sample-rate
  // shading, one pipeline per sample per entry. VK_NULL_HANDLE if failed to
  // create.
  std::unordered_map<TransferPipelineKey, std::array<VkPipeline, 4>, TransferPipelineKey::Hasher>
      transfer_pipelines_;

  VkPipelineLayout dump_pipeline_layout_color_ = VK_NULL_HANDLE;
  VkPipelineLayout dump_pipeline_layout_depth_ = VK_NULL_HANDLE;
  // Compute pipelines for copying host render target contents to the EDRAM
  // buffer. VK_NULL_HANDLE if failed to create.
  std::unordered_map<DumpPipelineKey, VkPipeline, DumpPipelineKey::Hasher> dump_pipelines_;
  VkPipelineLayout direct_resolve_pipeline_layout_color_ = VK_NULL_HANDLE;
  VkPipelineLayout direct_resolve_pipeline_layout_depth_ = VK_NULL_HANDLE;
  std::unordered_map<DirectResolvePipelineKey, VkPipeline, DirectResolvePipelineKey::Hasher>
      direct_resolve_pipelines_;

  // Temporary storage for Resolve.
  std::vector<Transfer> clear_transfers_[2];

  // Temporary storage for PerformTransfersAndResolveClears.
  std::vector<TransferInvocation> current_transfer_invocations_;

  // Transfers held back by clears (see FlushDeferredTransfers): the render
  // targets they go to (bit 0 depth, bits 1-4 color), the bindings when they
  // were held back, and the area cleared since.
  uint32_t deferred_transfer_targets_ = 0;
  RenderTarget* deferred_transfer_bindings_[1 + xenos::kMaxColorRenderTargets] = {};
  std::array<std::vector<Transfer>, 1 + xenos::kMaxColorRenderTargets> deferred_transfers_;
  Transfer::Rectangle deferred_transfer_cutout_;

  // Incremented when the contents of all render targets may change outside
  // draws and transfers (EDRAM snapshot restores), forgetting uniform stencils.
  uint64_t uniform_stencil_generation_ = 1;

  // Temporary storage for DumpRenderTargets.
  std::vector<ResolveCopyDumpRectangle> dump_rectangles_;
  std::vector<DumpInvocation> dump_invocations_;
  std::vector<ResolveCopyDispatch> direct_resolve_dispatches_;
  uint64_t direct_resolve_attempt_count_ = 0;
  uint64_t direct_resolve_success_count_ = 0;
  uint64_t direct_resolve_fallback_count_ = 0;

  bool native_resolve_enabled_ = false;
  uint32_t native_resolve_shared_memory_binding_count_ = 0;
  // Fragment shader quad subgroup operations are supported, and the depth
  // native resolve shader captures the stencil with them.
  bool native_resolve_quad_stencil_capture_ = false;

  // A native resolve whose scaled resolve memory hasn't been written: the data
  // is in the texture (kept alive while pending) - for color, in the raw bits
  // the memory would hold; for depth, as the converted depth the texture
  // samples, with the stencil the memory would also hold captured separately.
  struct PendingScaledResolveMemory {
    void* texture = nullptr;
    uint32_t dest_base = 0;
    uint32_t extent_start = 0;
    uint32_t extent_length = 0;
    uint32_t dest_pitch_texels = 0;
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint32_t memory_flags = 0;
    // Depth: the resolve's flags (depth format) and the index of the stencil
    // capture (scaled_stencil_captures_), in 2x2 quads of host pixels per dword
    // (written by the native resolve) or bytes (copied).
    bool is_depth = false;
    uint32_t flags = 0;
    uint32_t stencil_capture = UINT32_MAX;
    bool stencil_capture_quads = false;
    // Instead of a capture, the stencil of the whole rectangle (uniform),
    // filled into a capture when writing back.
    bool stencil_uniform = false;
    uint32_t stencil_uniform_value = 0;
    // native_resolve_debug_verify_stencil_capture: a copied capture the
    // write-back compares the quad capture with.
    uint32_t verify_stencil_capture = UINT32_MAX;
    // A texture written through a unorm view of its own format
    // (native_resolve_unorm_views): the NativeResolvePacking of its bits.
    uint32_t unorm_packing = UINT32_MAX;
    // Depth written through a float view of its own format.
    bool depth_float_view = false;
  };
  std::vector<PendingScaledResolveMemory> pending_scaled_resolve_memory_;
  VulkanTextureCache* pending_scaled_resolve_texture_cache_ = nullptr;
  // Sampled image descriptor sets (descriptor_set_pool_sampled_image_) used by
  // write-backs, with the submission using them, freed once it completes.
  std::deque<std::pair<uint64_t, size_t>> scaled_memory_writeback_descriptors_;
  VkShaderModule scaled_memory_writeback_shader_ = VK_NULL_HANDLE;
  VkPipelineLayout scaled_memory_writeback_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline scaled_memory_writeback_pipeline_ = VK_NULL_HANDLE;
  bool scaled_memory_writeback_pipeline_failed_ = false;
  bool EnsureScaledMemoryWritebackPipeline();
  // The depth write-back also reads the stencil capture (set 2).
  VkShaderModule scaled_memory_writeback_depth_shader_ = VK_NULL_HANDLE;
  VkPipelineLayout scaled_memory_writeback_depth_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline scaled_memory_writeback_depth_pipeline_ = VK_NULL_HANDLE;
  bool scaled_memory_writeback_depth_pipeline_failed_ = false;
  bool EnsureScaledMemoryWritebackDepthPipeline();
  // Color write-back reading a unorm view (native_resolve_unorm_views) and
  // packing it like the native resolve; the color write-back's layout.
  VkShaderModule scaled_memory_writeback_unorm_shader_ = VK_NULL_HANDLE;
  VkPipeline scaled_memory_writeback_unorm_pipeline_ = VK_NULL_HANDLE;
  bool scaled_memory_writeback_unorm_pipeline_failed_ = false;
  bool EnsureScaledMemoryWritebackUnormPipeline();
  // Depth write-back reading a float view of the texture's own format; the
  // depth write-back's layout.
  VkShaderModule scaled_memory_writeback_depth_float_shader_ = VK_NULL_HANDLE;
  VkPipeline scaled_memory_writeback_depth_float_pipeline_ = VK_NULL_HANDLE;
  bool scaled_memory_writeback_depth_float_pipeline_failed_ = false;
  bool EnsureScaledMemoryWritebackDepthFloatPipeline();
  // The stencil of a lazily written depth resolve, one byte per host texel of
  // the resolved rectangle, row by row.
  struct ScaledStencilCapture {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    bool in_use = false;
  };
  std::vector<ScaledStencilCapture> scaled_stencil_captures_;
  // Index of a capture not held by a pending resolve, of at least `size` bytes
  // (created if needed), or UINT32_MAX if there are too many. Captures are
  // reused right away: the copy into one waits for earlier accesses to it.
  uint32_t AcquireScaledStencilCapture(VkDeviceSize size);
  void ReleaseScaledStencilCapture(uint32_t index);
  // Copies the stencil of the resolved rectangle of the source to a capture.
  void CaptureScaledResolveStencil(VulkanRenderTarget& source, uint32_t x0, uint32_t y0,
                                   uint32_t x1, uint32_t y1, uint32_t capture_index);
  // Fills the first size bytes of a capture with one stencil value, laid out
  // like a copied capture (a byte per texel).
  void FillScaledStencilCapture(uint32_t capture_index, uint32_t value, VkDeviceSize size);
  // Releases what a pending resolve holds without writing it back.
  void DropPendingScaledResolveMemory(const PendingScaledResolveMemory& pending);
  // Records the write-back of one pending resolve and releases its texture.
  void WriteBackPendingScaledResolveMemory(const PendingScaledResolveMemory& pending);
  // Before a resolve writes [start, start + length): pending resolves it fully
  // covers are dropped (superseded), partially overlapped ones written back.
  void PrepareScaledResolveMemoryForResolve(uint32_t start, uint32_t length);
  VkShaderModule native_resolve_vertex_shader_ = VK_NULL_HANDLE;
  VkShaderModule native_resolve_fragment_shaders_[size_t(NativeResolveShader::kCount)] = {};
  VkPipelineLayout native_resolve_pipeline_layout_color_ = VK_NULL_HANDLE;
  VkPipelineLayout native_resolve_pipeline_layout_depth_ = VK_NULL_HANDLE;
  // Keyed by (destination format << 8) | shader. VK_NULL_HANDLE if failed to
  // create.
  std::unordered_map<uint64_t, VkPipeline> native_resolve_pipelines_;
  // Only the extent is used - with dynamic rendering, no framebuffer object is
  // needed for rendering into a texture view.
  Framebuffer native_resolve_framebuffer_;
  std::vector<ResolveCopyDumpRectangle> native_resolve_rectangles_;
  uint64_t native_resolve_texture_write_count_ = 0;

  // For traces.
  VkBuffer edram_snapshot_download_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory edram_snapshot_download_buffer_memory_ = VK_NULL_HANDLE;
  uint32_t edram_snapshot_download_buffer_memory_type_ = UINT32_MAX;
  VkDeviceSize edram_snapshot_download_buffer_memory_size_ = 0;
  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> edram_snapshot_restore_pool_;
  // Guest memory bytes for native resolve buffers (UploadGuestMemoryToBuffer).
  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> native_resolve_upload_pool_;
  void ResetTraceDownload();

  // For pixel (fragment) shader interlock.

  VkRenderPass fsi_render_pass_ = VK_NULL_HANDLE;
  Framebuffer fsi_framebuffer_;

  VkPipelineLayout resolve_fsi_clear_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline resolve_fsi_clear_32bpp_pipeline_ = VK_NULL_HANDLE;
  VkPipeline resolve_fsi_clear_64bpp_pipeline_ = VK_NULL_HANDLE;
};

}  // namespace rex::graphics::vulkan
