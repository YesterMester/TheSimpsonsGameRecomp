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

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <bitset>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include <rex/chrono/clock.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/native_records.h>
#include <rex/graphics/ring_progress.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/sampler_info.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/memory/ring_buffer.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>

#if REX_PLATFORM_LINUX
#include <sys/resource.h>
#endif

namespace rex::memory {
// Defined in xmemory.cpp, for the gpu_wait_stats log.
extern std::atomic<uint64_t> g_watch_protect_calls;
extern std::atomic<uint64_t> g_watch_protect_ticks;
extern std::atomic<uint64_t> g_watch_fault_count;
extern std::atomic<uint64_t> g_watch_fault_ticks;
extern std::atomic<uint32_t> g_watch_region_protects[512];
extern std::atomic<uint32_t> g_watch_region_protect_pages[512];
extern std::atomic<uint32_t> g_watch_region_faults[512];
extern std::atomic<uint32_t> g_watch_view_protects[3];
}  // namespace rex::memory

namespace rex::graphics {
// Defined in shared_memory.cpp, for the gpu_wait_stats log.
extern std::atomic<uint64_t> g_streamed_page_uploads;
extern std::atomic<uint64_t> g_streamed_page_max_uploads;
extern std::atomic<uint64_t> g_streamed_pages_over[4];
}  // namespace rex::graphics

namespace rex::ui {
// Defined in presenter.cpp, for the gpu_wait_stats log.
extern std::atomic<uint64_t> g_host_presents;
extern std::atomic<uint64_t> g_present_max_ticks[10];
extern std::atomic<uint64_t> g_present_total_ticks[10];
}  // namespace rex::ui

#if REX_HAS_VULKAN
namespace rex::graphics::vulkan {
// Defined in vulkan/command_processor.cpp and vulkan/render_target_cache.cpp,
// for the gpu_wait_stats log.
extern std::atomic<uint64_t> g_submission_build_ticks;
extern std::atomic<uint64_t> g_submission_submit_ticks;
extern std::atomic<uint64_t> g_submission_count;
extern std::atomic<uint64_t> g_memexport_readback_ticks;
extern std::atomic<uint64_t> g_stencil_enabled_draws;
extern std::atomic<uint64_t> g_stencil_write_draws;
extern std::atomic<uint64_t> g_stencil_nonzero_clears;
}  // namespace rex::graphics::vulkan
#endif  // REX_HAS_VULKAN

REXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");

REXCVAR_DEFINE_BOOL(gpu_incremental_read_pointer, false, "GPU",
                    "Write the ring buffer read pointer back after every primary packet instead "
                    "of once per batch, so the game can reuse command memory sooner");

REXCVAR_DEFINE_INT32(gpu_wait_stats, 0, "GPU",
                     "Log where the command processor thread spends its time (waiting for the "
                     "game, WAIT_REG_MEM, host GPU fences, swaps) every N frames "
                     "(0 = off; diagnostic)");

REXCVAR_DEFINE_BOOL(clear_memory_page_state, false, "GPU",
                    "At every frame end, forget which guest memory pages the GPU has an "
                    "up-to-date copy of (except GPU-written ones), so the next frame uploads "
                    "every vertex and index buffer it uses again. Write watches already catch "
                    "CPU writes; this costs about 4 ms of GPU time per frame in Springfield at 2x "
                    "(hundreds of uploads that each wait for the GPU to go idle).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(occlusion_query_enable, true, "GPU", "Enable host occlusion query handling")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(readback_resolve, "none", "GPU",
                      "Controls CPU readback of render-to-texture resolve results.\n"
                      " none: Disable readback (default)\n"
                      " fast: Read previous frame (delayed, copy every frame)\n"
                      " some: Read previous frame (delayed, copy on cache miss)\n"
                      " full: Immediate sync readback (accurate but stalls)")
    .allowed({"none", "fast", "some", "full"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_resolve_half_pixel_offset, false, "GPU",
                    "When draw resolution scaling is active, sample from the center of each "
                    "scaled block during resolve readback downscale")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_memexport, true, "GPU",
                    "Enable CPU readback of shader memexport writes for guest memory "
                    "coherency (can reduce correctness issues, but may add GPU/CPU sync cost)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_memexport_fast, true, "GPU",
                    "Use fast double-buffered memexport readback when possible, with "
                    "automatic fallback to full synchronous readback")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(query_occlusion_fake_sample_count, 1000, "GPU",
                     "Fake sample count for occlusion queries")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(async_shader_compilation, true, "GPU",
                    "Compile shaders and create pipelines asynchronously in background "
                    "threads. This reduces stutter but may cause brief visual artifacts while "
                    "pipelines are being prepared.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics {

using namespace rex::graphics::xenos;

namespace {
// Ring progress written back to guest memory (read pointer and scratch
// register writebacks), for guest code waiting for the ring to be consumed (see
// ring_progress.h). The mutex and condition variable are only touched when
// someone waits.
std::atomic<uint32_t> g_read_pointer_writebacks{0};
std::atomic<uint32_t> g_read_pointer_waiters{0};
std::atomic<uint32_t> g_read_pointer_writeback_address{0};
std::mutex g_read_pointer_mutex;
std::condition_variable g_read_pointer_condition;

void NotifyRingProgress() {
  g_read_pointer_writebacks.fetch_add(1, std::memory_order_seq_cst);
  if (g_read_pointer_waiters.load(std::memory_order_seq_cst)) {
    std::lock_guard<std::mutex> lock(g_read_pointer_mutex);
    g_read_pointer_condition.notify_all();
  }
}
}  // namespace

uint32_t GetRingProgressCount() {
  return g_read_pointer_writebacks.load(std::memory_order_seq_cst);
}

void WaitForRingProgress(uint32_t seen_count, uint32_t timeout_us) {
  // Registered before checking the count, and the notifier increments the
  // count before checking for waiters, so a writeback can't be missed.
  g_read_pointer_waiters.fetch_add(1, std::memory_order_seq_cst);
  {
    std::unique_lock<std::mutex> lock(g_read_pointer_mutex);
    g_read_pointer_condition.wait_for(lock, std::chrono::microseconds(timeout_us), [&] {
      return g_read_pointer_writebacks.load(std::memory_order_seq_cst) != seen_count;
    });
  }
  g_read_pointer_waiters.fetch_sub(1, std::memory_order_seq_cst);
}

uint32_t GetReadPointerWritebackAddress() {
  return g_read_pointer_writeback_address.load(std::memory_order_relaxed);
}

namespace {

// gpu_wait_stats: where the command processor thread's time goes, in host
// ticks, accumulated between logs. Only the command processor thread writes
// these, except the fence counters, which the backend may report from
// elsewhere.
struct GpuWaitSite {
  uint32_t address = 0;
  uint32_t wait_info = 0;
  uint32_t mask = 0;
  uint32_t ref = 0;
  uint32_t interval = 0;
  uint32_t unmet_value = 0;
  uint64_t packets = 0;
  uint64_t unmet = 0;
  uint64_t in_indirect = 0;
  uint64_t ticks = 0;
  uint64_t max_ticks = 0;
};
struct GpuWaitStats {
  uint64_t ring_idle_waits = 0;
  uint64_t ring_idle_ticks = 0;
  uint64_t wait_packets = 0;
  uint64_t wait_unmet = 0;
  uint64_t wait_sleeps = 0;
  uint64_t wait_ticks = 0;
  uint64_t swap_ticks = 0;
  uint64_t max_swap_ticks = 0;
  static constexpr uint32_t kMaxSites = 24;
  GpuWaitSite sites[kMaxSites];
  uint32_t site_count = 0;
  uint64_t untracked_site_packets = 0;
};
GpuWaitStats g_gpu_wait_stats;
std::atomic<uint64_t> g_gpu_fence_waits{0}, g_gpu_fence_ticks{0};
std::atomic<uint64_t> g_gpu_full_sync_waits{0}, g_gpu_full_sync_ticks{0};
uint32_t g_gpu_wait_stats_frames = 0;
uint64_t g_gpu_wait_stats_start_tick = 0;
// CPU time of the command processor thread at the start of the window, ms.
double g_gpu_wait_stats_user_ms = 0.0;
double g_gpu_wait_stats_system_ms = 0.0;

void GetCommandProcessorThreadTimes(double& user_ms, double& system_ms) {
  user_ms = system_ms = 0.0;
#if REX_PLATFORM_LINUX
  struct rusage usage;
  if (getrusage(RUSAGE_THREAD, &usage) == 0) {
    user_ms = double(usage.ru_utime.tv_sec) * 1000.0 + double(usage.ru_utime.tv_usec) / 1000.0;
    system_ms = double(usage.ru_stime.tv_sec) * 1000.0 + double(usage.ru_stime.tv_usec) / 1000.0;
  }
#endif
}

void RecordGpuWait(uint32_t address, uint32_t wait_info, uint32_t mask, uint32_t ref,
                   uint32_t interval, bool in_indirect, bool unmet, uint32_t unmet_value,
                   uint64_t unmet_ticks) {
  GpuWaitStats& s = g_gpu_wait_stats;
  ++s.wait_packets;
  if (unmet) {
    ++s.wait_unmet;
    s.wait_ticks += unmet_ticks;
  }
  GpuWaitSite* site = nullptr;
  for (uint32_t i = 0; i < s.site_count; ++i) {
    GpuWaitSite& candidate = s.sites[i];
    if (candidate.address == address && candidate.wait_info == wait_info &&
        candidate.mask == mask) {
      site = &candidate;
      break;
    }
  }
  if (!site) {
    if (s.site_count >= GpuWaitStats::kMaxSites) {
      ++s.untracked_site_packets;
      return;
    }
    site = &s.sites[s.site_count++];
    site->address = address;
    site->wait_info = wait_info;
    site->mask = mask;
  }
  ++site->packets;
  site->ref = ref;
  site->interval = interval;
  site->in_indirect += in_indirect ? 1 : 0;
  if (unmet) {
    ++site->unmet;
    site->unmet_value = unmet_value;
    site->ticks += unmet_ticks;
    site->max_ticks = std::max(site->max_ticks, unmet_ticks);
  }
}

void LogGpuWaitStats(uint32_t interval) {
  uint64_t now = rex::chrono::Clock::QueryHostTickCount();
  if (!g_gpu_wait_stats_start_tick) {
    // Start the first window at a swap.
    g_gpu_wait_stats = GpuWaitStats();
    g_gpu_fence_waits = g_gpu_fence_ticks = 0;
    g_gpu_full_sync_waits = g_gpu_full_sync_ticks = 0;
    g_gpu_wait_stats_frames = 0;
    g_gpu_wait_stats_start_tick = now;
    rex::memory::g_watch_protect_calls = rex::memory::g_watch_protect_ticks = 0;
    rex::memory::g_watch_fault_count = rex::memory::g_watch_fault_ticks = 0;
    g_streamed_page_uploads = 0;
    rex::ui::g_host_presents = 0;
    GetCommandProcessorThreadTimes(g_gpu_wait_stats_user_ms, g_gpu_wait_stats_system_ms);
    return;
  }
  if (++g_gpu_wait_stats_frames < interval) {
    return;
  }
  const GpuWaitStats& s = g_gpu_wait_stats;
  const double ms_per_tick = 1000.0 / double(rex::chrono::Clock::QueryHostTickFrequency());
  const double frames = double(g_gpu_wait_stats_frames);
  auto per_frame_ms = [&](uint64_t ticks) { return double(ticks) * ms_per_tick / frames; };
  uint64_t fence_waits = g_gpu_fence_waits.exchange(0);
  uint64_t fence_ticks = g_gpu_fence_ticks.exchange(0);
  uint64_t full_sync_waits = g_gpu_full_sync_waits.exchange(0);
  uint64_t full_sync_ticks = g_gpu_full_sync_ticks.exchange(0);
  uint64_t host_presents = rex::ui::g_host_presents.exchange(0);
  REXGPU_INFO(
      "[gpu-wait] last {} frames, {:.2f} ms/frame: waiting for the game {:.2f} ms ({:.1f} "
      "waits), WAIT_REG_MEM {:.2f} ms ({:.1f} packets, {:.1f} unmet, {:.1f} sleeps), host GPU "
      "fences {:.2f} ms ({:.1f} waits), full syncs {:.2f} ms ({:.1f}), swap {:.2f} ms (max "
      "{:.2f}), {:.2f} host presents per frame",
      g_gpu_wait_stats_frames, per_frame_ms(now - g_gpu_wait_stats_start_tick),
      per_frame_ms(s.ring_idle_ticks), double(s.ring_idle_waits) / frames,
      per_frame_ms(s.wait_ticks), double(s.wait_packets) / frames,
      double(s.wait_unmet) / frames, double(s.wait_sleeps) / frames, per_frame_ms(fence_ticks),
      double(fence_waits) / frames, per_frame_ms(full_sync_ticks),
      double(full_sync_waits) / frames, per_frame_ms(s.swap_ticks),
      double(s.max_swap_ticks) * ms_per_tick, double(host_presents) / frames);
  // The sites that cost the most time, then the most frequent.
  std::vector<const GpuWaitSite*> sites;
  for (uint32_t i = 0; i < s.site_count; ++i) {
    sites.push_back(&s.sites[i]);
  }
  std::sort(sites.begin(), sites.end(), [](const GpuWaitSite* a, const GpuWaitSite* b) {
    return a->ticks != b->ticks ? a->ticks > b->ticks : a->packets > b->packets;
  });
  static const char* const kFunctions[] = {"never", "<", "<=", "==", "!=", ">=", ">", "always"};
  for (size_t i = 0; i < std::min(sites.size(), size_t(8)); ++i) {
    const GpuWaitSite& site = *sites[i];
    REXGPU_INFO(
        "[gpu-wait]   {} {:08X} & {:08X} {} {:08X} (interval {:X}): {:.1f}/frame, {:.1f} unmet "
        "({:.3f} ms/frame, max {:.3f} ms, last unmet value {:08X}), {:.0f}% in indirect buffers",
        (site.wait_info & 0x10) ? "mem" : "reg", site.address, site.mask,
        kFunctions[site.wait_info & 7], site.ref, site.interval, double(site.packets) / frames,
        double(site.unmet) / frames, per_frame_ms(site.ticks),
        double(site.max_ticks) * ms_per_tick, site.unmet_value,
        100.0 * double(site.in_indirect) / double(site.packets));
  }
  if (s.untracked_site_packets) {
    REXGPU_INFO("[gpu-wait]   {} packets at untracked sites", s.untracked_site_packets);
  }
  // Write watches on guest memory the GPU has read: protection changes to arm
  // them (made by the command processor thread) and the guest write faults
  // that fire them (on the writing threads). Plus the command processor
  // thread's own CPU time split into user and kernel.
  uint64_t protect_calls = rex::memory::g_watch_protect_calls.exchange(0);
  uint64_t protect_ticks = rex::memory::g_watch_protect_ticks.exchange(0);
  uint64_t fault_count = rex::memory::g_watch_fault_count.exchange(0);
  uint64_t fault_ticks = rex::memory::g_watch_fault_ticks.exchange(0);
  double user_ms, system_ms;
  GetCommandProcessorThreadTimes(user_ms, system_ms);
  uint64_t streamed_page_uploads = g_streamed_page_uploads.exchange(0);
  REXGPU_INFO(
      "[gpu-wait]   write watches: {:.1f} protects ({:.3f} ms), {:.1f} guest write faults "
      "({:.3f} ms in handlers), {:.1f} streamed pages uploaded unwatched | command processor "
      "thread CPU: user {:.2f} ms, kernel {:.2f} ms",
      double(protect_calls) / frames, per_frame_ms(protect_ticks), double(fault_count) / frames,
      per_frame_ms(fault_ticks), double(streamed_page_uploads) / frames,
      (user_ms - g_gpu_wait_stats_user_ms) / frames,
      (system_ms - g_gpu_wait_stats_system_ms) / frames);
  // The 1 MB physical memory regions with the most watch protect calls.
  std::vector<std::pair<uint32_t, uint32_t>> regions;
  uint32_t region_protects[512], region_pages[512], region_faults[512];
  for (uint32_t i = 0; i < 512; ++i) {
    region_protects[i] = rex::memory::g_watch_region_protects[i].exchange(0);
    region_pages[i] = rex::memory::g_watch_region_protect_pages[i].exchange(0);
    region_faults[i] = rex::memory::g_watch_region_faults[i].exchange(0);
    if (region_protects[i] || region_faults[i]) {
      regions.emplace_back(region_protects[i] + region_faults[i], i);
    }
  }
  std::sort(regions.rbegin(), regions.rend());
  std::string region_text;
  for (size_t i = 0; i < std::min(regions.size(), size_t(8)); ++i) {
    uint32_t r = regions[i].second;
    region_text += fmt::format(" {:03X}xxxxx: {:.1f} protects ({:.1f} pages), {:.1f} faults |", r,
                               double(region_protects[r]) / frames,
                               double(region_pages[r]) / frames,
                               double(region_faults[r]) / frames);
  }
  uint32_t view_protects[3];
  for (uint32_t i = 0; i < 3; ++i) {
    view_protects[i] = rex::memory::g_watch_view_protects[i].exchange(0);
  }
  REXGPU_INFO("[gpu-wait]   watch protects by view A/C/E: {:.1f}/{:.1f}/{:.1f}, by region:{}",
              double(view_protects[0]) / frames, double(view_protects[1]) / frames,
              double(view_protects[2]) / frames, region_text);
  uint64_t present_max[10];
  for (uint32_t i = 0; i < 10; ++i) {
    present_max[i] = rex::ui::g_present_max_ticks[i].exchange(0);
  }
  REXGPU_INFO(
      "[gpu-wait]   presenter longest: guest output refresh {:.2f} ms, paint {:.2f} ms (old "
      "paint submission {:.2f}, acquire {:.2f}, recording {:.2f}, fence {:.2f}, queue lock "
      "{:.2f}, submit {:.2f}, present {:.2f}, other waits {:.2f})",
      double(present_max[4]) * ms_per_tick, double(present_max[5]) * ms_per_tick,
      double(present_max[0]) * ms_per_tick, double(present_max[1]) * ms_per_tick,
      double(present_max[9]) * ms_per_tick, double(present_max[6]) * ms_per_tick,
      double(present_max[7]) * ms_per_tick, double(present_max[8]) * ms_per_tick,
      double(present_max[2]) * ms_per_tick, double(present_max[3]) * ms_per_tick);
#if REX_HAS_VULKAN
  {
    uint64_t present_total[10];
    for (uint32_t i = 0; i < 10; ++i) {
      present_total[i] = rex::ui::g_present_total_ticks[i].exchange(0);
    }
    uint64_t build_ticks = vulkan::g_submission_build_ticks.exchange(0);
    uint64_t submit_ticks = vulkan::g_submission_submit_ticks.exchange(0);
    uint64_t submission_count = vulkan::g_submission_count.exchange(0);
    uint64_t readback_ticks = vulkan::g_memexport_readback_ticks.exchange(0);
    REXGPU_INFO(
        "[gpu-wait]   swap parts per frame: memexport readback {:.2f} ms, submission build "
        "{:.2f} ms ({:.1f} submissions), vkQueueSubmit {:.2f} ms, presenter: guest output "
        "refresh {:.2f} ms, paint {:.2f} ms (acquire {:.2f}, recording {:.2f}, submit {:.2f}, "
        "present {:.2f})",
        per_frame_ms(readback_ticks), per_frame_ms(build_ticks),
        double(submission_count) / frames, per_frame_ms(submit_ticks),
        per_frame_ms(present_total[4]), per_frame_ms(present_total[5]),
        per_frame_ms(present_total[1]), per_frame_ms(present_total[9]),
        per_frame_ms(present_total[8]), per_frame_ms(present_total[2]));
    REXGPU_INFO(
        "[gpu-wait]   stencil: {:.1f} draws per frame with the stencil test, {:.1f} of them can "
        "write it, {:.1f} resolve clears with a non-zero stencil value",
        double(vulkan::g_stencil_enabled_draws.exchange(0)) / frames,
        double(vulkan::g_stencil_write_draws.exchange(0)) / frames,
        double(vulkan::g_stencil_nonzero_clears.exchange(0)) / frames);
  }
#endif  // REX_HAS_VULKAN
  uint64_t streamed_max = g_streamed_page_max_uploads.exchange(0);
  uint64_t streamed_over[4];
  for (uint32_t i = 0; i < 4; ++i) {
    streamed_over[i] = g_streamed_pages_over[i].exchange(0);
  }
  REXGPU_INFO(
      "[gpu-wait]   streamed page uploads per frame: most for one page {:.1f}, pages over 8 "
      "{:.1f}, over 16 {:.1f}, over 32 {:.1f}, at the cap {:.1f}",
      double(streamed_max) / frames, double(streamed_over[0]) / frames,
      double(streamed_over[1]) / frames, double(streamed_over[2]) / frames,
      double(streamed_over[3]) / frames);
  g_gpu_wait_stats_user_ms = user_ms;
  g_gpu_wait_stats_system_ms = system_ms;
  g_gpu_wait_stats = GpuWaitStats();
  g_gpu_wait_stats_frames = 0;
  g_gpu_wait_stats_start_tick = now;
}

ReadbackResolveMode ParseReadbackResolveMode(std::string_view value) {
  if (value == "fast") {
    return ReadbackResolveMode::kFast;
  }
  if (value == "some") {
    return ReadbackResolveMode::kSome;
  }
  if (value == "full") {
    return ReadbackResolveMode::kFull;
  }
  return ReadbackResolveMode::kDisabled;
}

// Documented registers, for the unknown-register diagnostic in WriteRegister.
// A table instead of RegisterFile::GetRegisterInfo's switch because that check
// runs on every register write.
std::bitset<RegisterFile::kRegisterCount> BuildKnownRegisterSet() {
  std::bitset<RegisterFile::kRegisterCount> known;
#define XE_GPU_REGISTER(index, type, name) known.set(index);
#include <rex/graphics/register_table.inc>
#undef XE_GPU_REGISTER
  return known;
}

const std::bitset<RegisterFile::kRegisterCount> kKnownRegisters = BuildKnownRegisterSet();

}  // namespace

CommandProcessor::CommandProcessor(GraphicsSystem* graphics_system,
                                   system::KernelState* kernel_state)
    : memory_(graphics_system->memory()),
      kernel_state_(kernel_state),
      graphics_system_(graphics_system),
      register_file_(graphics_system_->register_file()),
      trace_writer_(graphics_system->memory()->physical_membase()),
      worker_running_(true),
      write_ptr_index_event_(rex::thread::Event::CreateAutoResetEvent(false)),
      write_ptr_index_(0) {
  assert_not_null(write_ptr_index_event_);
}

CommandProcessor::~CommandProcessor() = default;

bool CommandProcessor::Initialize() {
  // Initialize the gamma ramps to their default (linear) values - taken from
  // what games set when starting with the sRGB (return value 1)
  // VdGetCurrentDisplayGamma.
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = i * 0x3FF / 0xFF;
    reg::DC_LUT_30_COLOR& gamma_ramp_entry = gamma_ramp_256_entry_table_[i];
    gamma_ramp_entry.color_10_blue = value;
    gamma_ramp_entry.color_10_green = value;
    gamma_ramp_entry.color_10_red = value;
  }
  for (uint32_t i = 0; i < 128; ++i) {
    reg::DC_LUT_PWL_DATA gamma_ramp_entry = {};
    gamma_ramp_entry.base = (i * 0xFFFF / 0x7F) & ~UINT32_C(0x3F);
    gamma_ramp_entry.delta = i < 0x7F ? 0x200 : 0;
    for (uint32_t j = 0; j < 3; ++j) {
      gamma_ramp_pwl_rgb_[i][j] = gamma_ramp_entry;
    }
  }

  worker_running_ = true;
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));
  worker_thread_->set_name("GPU Commands");
  worker_thread_->Create();

  // Direct3D hooks may replace packets with native records from now on.
  native_records::Clear();
  native_records::SetEnabled(true);

  return true;
}

void CommandProcessor::Shutdown() {
  native_records::SetEnabled(false);
  EndTracing();

  worker_running_ = false;
  write_ptr_index_event_->Set();
  worker_thread_->Wait(0, 0, 0, nullptr);
  worker_thread_.reset();
}

void CommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                               uint32_t title_id, bool blocking) {}

void CommandProcessor::RequestFrameTrace(const std::filesystem::path& root_path) {
  if (trace_state_ == TraceState::kStreaming) {
    REXGPU_ERROR("Streaming trace; cannot also trace frame.");
    return;
  }
  if (trace_state_ == TraceState::kSingleFrame) {
    REXGPU_ERROR("Frame trace already pending; ignoring.");
    return;
  }
  trace_state_ = TraceState::kSingleFrame;
  trace_frame_path_ = root_path;
}

void CommandProcessor::BeginTracing(const std::filesystem::path& root_path) {
  if (trace_state_ == TraceState::kStreaming) {
    REXGPU_ERROR("Streaming already active; ignoring request.");
    return;
  }
  if (trace_state_ == TraceState::kSingleFrame) {
    REXGPU_ERROR("Frame trace pending; ignoring streaming request.");
    return;
  }
  // Streaming starts on the next primary buffer execute.
  trace_state_ = TraceState::kStreaming;
  trace_stream_path_ = root_path;
}

void CommandProcessor::EndTracing() {
  if (!trace_writer_.is_open()) {
    return;
  }
  assert_true(trace_state_ == TraceState::kStreaming);
  trace_state_ = TraceState::kDisabled;
  trace_writer_.Close();
}

void CommandProcessor::RestoreRegisters(uint32_t first_register, const uint32_t* register_values,
                                        uint32_t register_count, bool execute_callbacks) {
  if (first_register > RegisterFile::kRegisterCount ||
      RegisterFile::kRegisterCount - first_register < register_count) {
    REXGPU_WARN(
        "CommandProcessor::RestoreRegisters out of bounds (0x{:X} registers "
        "starting with 0x{:X}, while a total of 0x{:X} registers are stored)",
        register_count, first_register, RegisterFile::kRegisterCount);
    if (first_register > RegisterFile::kRegisterCount) {
      return;
    }
    register_count =
        std::min(uint32_t(RegisterFile::kRegisterCount) - first_register, register_count);
  }
  if (execute_callbacks) {
    for (uint32_t i = 0; i < register_count; ++i) {
      WriteRegister(first_register + i, register_values[i]);
    }
  } else {
    std::memcpy(register_file_->values + first_register, register_values,
                sizeof(uint32_t) * register_count);
  }
}

void CommandProcessor::RestoreGammaRamp(const reg::DC_LUT_30_COLOR* new_gamma_ramp_256_entry_table,
                                        const reg::DC_LUT_PWL_DATA* new_gamma_ramp_pwl_rgb,
                                        uint32_t new_gamma_ramp_rw_component) {
  std::memcpy(gamma_ramp_256_entry_table_, new_gamma_ramp_256_entry_table,
              sizeof(reg::DC_LUT_30_COLOR) * 256);
  std::memcpy(gamma_ramp_pwl_rgb_, new_gamma_ramp_pwl_rgb, sizeof(reg::DC_LUT_PWL_DATA) * 3 * 128);
  gamma_ramp_rw_component_ = new_gamma_ramp_rw_component;
  OnGammaRamp256EntryTableValueWritten();
  OnGammaRampPWLValueWritten();
}

void CommandProcessor::CallInThread(std::function<void()> fn) {
  if (pending_fns_.empty() && system::XThread::IsInThread(worker_thread_.get())) {
    fn();
  } else {
    pending_fns_.push(std::move(fn));
  }
}

void CommandProcessor::ClearCaches() {}

void CommandProcessor::InvalidateGpuMemory() {}

ReadbackResolveMode CommandProcessor::GetReadbackResolveMode(
    bool legacy_readback_resolve_enabled) const {
  ReadbackResolveMode shared_mode = ParseReadbackResolveMode(REXCVAR_GET(readback_resolve));
  bool shared_mode_overrides_legacy = shared_mode != ReadbackResolveMode::kDisabled ||
                                      rex::cvar::HasNonDefaultValue("readback_resolve");
  if (shared_mode_overrides_legacy) {
    return shared_mode;
  }
  return legacy_readback_resolve_enabled ? ReadbackResolveMode::kFast
                                         : ReadbackResolveMode::kDisabled;
}

bool CommandProcessor::IsReadbackMemexportEnabled(bool legacy_backend_flag) const {
  if (legacy_readback_memexport_overridden_ < 0) {
    legacy_readback_memexport_overridden_ =
        (legacy_readback_memexport_cvar_name_ &&
         rex::cvar::HasNonDefaultValue(legacy_readback_memexport_cvar_name_))
            ? 1
            : 0;
  }
  if (legacy_readback_memexport_overridden_) {
    return legacy_backend_flag;
  }
  return REXCVAR_GET(readback_memexport);
}

void CommandProcessor::SetDesiredSwapPostEffect(SwapPostEffect swap_post_effect) {
  if (swap_post_effect_desired_ == swap_post_effect) {
    return;
  }
  swap_post_effect_desired_ = swap_post_effect;
  CallInThread([this, swap_post_effect]() { swap_post_effect_actual_ = swap_post_effect; });
}

void CommandProcessor::WorkerThreadMain() {
  if (!SetupContext()) {
    rex::FatalError("Unable to setup command processor internal state");
    return;
  }

  while (worker_running_) {
    while (!pending_fns_.empty()) {
      auto fn = std::move(pending_fns_.front());
      pending_fns_.pop();
      fn();
    }

    uint32_t write_ptr_index = write_ptr_index_.load();
    if (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index) {
      SCOPE_profile_cpu_i("gpu", "rex::graphics::CommandProcessor::Stall");
      // We've run out of commands to execute.
      // HAND PATCH: the old policy here was 500 sched-yield spins before ever
      // touching the event, then a 5 ms timed wait. That made sense for an
      // emulator whose GPU thread had a core to itself; in this recomp the
      // same cores also run the recompiled game code, and a yield storm on
      // every ring drain steals cycles from the thread that is about to feed
      // us more commands. UpdateWritePointer signals the event, so a real
      // wait wakes in microseconds; keep only a short yield burst to catch
      // back-to-back submissions, then sleep on the event. The 1 ms timeout
      // (down from 5) bounds how long a CallInThread posted from another
      // thread can sit unnoticed, since those don't signal the event.
      PrepareForWait();
      const bool wait_stats = REXCVAR_GET(gpu_wait_stats) > 0;
      uint64_t idle_start_tick = wait_stats ? rex::chrono::Clock::QueryHostTickCount() : 0;
      uint32_t loop_count = 0;
      do {
        if (loop_count < 32) {
          rex::thread::MaybeYield();
          loop_count++;
        } else {
          rex::thread::Wait(write_ptr_index_event_.get(), true,
                            std::chrono::milliseconds(1));
        }
        write_ptr_index = write_ptr_index_.load();
      } while (worker_running_ && pending_fns_.empty() &&
               (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index));
      if (wait_stats) {
        ++g_gpu_wait_stats.ring_idle_waits;
        g_gpu_wait_stats.ring_idle_ticks +=
            rex::chrono::Clock::QueryHostTickCount() - idle_start_tick;
      }
      ReturnFromWait();
      if (!worker_running_ || !pending_fns_.empty()) {
        continue;
      }
    }
    assert_true(read_ptr_index_ != write_ptr_index);

    // Execute. Note that we handle wraparound transparently.
    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);

    // TODO(benvanik): use reader->Read_update_freq_ and only issue after moving
    //     that many indices.
    if (read_ptr_writeback_ptr_) {
      memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_ptr_),
                                       read_ptr_index_);
      NotifyRingProgress();
    }

    // FIXME: We're supposed to process the WAIT_UNTIL register at this point,
    // but no games seem to actually use it.
  }

  ShutdownContext();
}

void CommandProcessor::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  thread::Fence fence;
  CallInThread([&fence]() {
    fence.Signal();
    thread::Thread::GetCurrentThread()->Suspend();
  });

  fence.Wait();
}

void CommandProcessor::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  worker_thread_->thread()->Resume();
}

bool CommandProcessor::Save(::rex::stream::ByteStream* stream) {
  assert_true(paused_);

  stream->Write<uint32_t>(primary_buffer_ptr_);
  stream->Write<uint32_t>(primary_buffer_size_);
  stream->Write<uint32_t>(read_ptr_index_);
  stream->Write<uint32_t>(read_ptr_update_freq_);
  stream->Write<uint32_t>(read_ptr_writeback_ptr_);
  stream->Write<uint32_t>(write_ptr_index_.load());

  return true;
}

bool CommandProcessor::Restore(::rex::stream::ByteStream* stream) {
  assert_true(paused_);

  primary_buffer_ptr_ = stream->Read<uint32_t>();
  primary_buffer_size_ = stream->Read<uint32_t>();
  read_ptr_index_ = stream->Read<uint32_t>();
  read_ptr_update_freq_ = stream->Read<uint32_t>();
  read_ptr_writeback_ptr_ = stream->Read<uint32_t>();
  write_ptr_index_.store(stream->Read<uint32_t>());

  return true;
}

bool CommandProcessor::SetupContext() {
  return true;
}

void CommandProcessor::ShutdownContext() {}

void CommandProcessor::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  read_ptr_index_ = 0;
  primary_buffer_ptr_ = ptr;
  primary_buffer_size_ = uint32_t(1) << (size_log2 + 3);
}

void CommandProcessor::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  // CP_RB_RPTR_ADDR Ring Buffer Read Pointer Address 0x70C
  // ptr = RB_RPTR_ADDR, pointer to write back the address to.
  read_ptr_writeback_ptr_ = ptr;
  g_read_pointer_writeback_address.store(ptr, std::memory_order_relaxed);
  // CP_RB_CNTL Ring Buffer Control 0x704
  // block_size = RB_BLKSZ, log2 of number of quadwords read between updates of
  //              the read pointer.
  read_ptr_update_freq_ = uint32_t(1) << block_size_log2 >> 2;
}

void CommandProcessor::UpdateWritePointer(uint32_t value) {
  write_ptr_index_ = value;
  write_ptr_index_event_->Set();
}

uint32_t CommandProcessor::ReadRegisterValue(uint32_t index) const {
  if (index < RegisterFile::kRegisterCount) {
    return register_file_->values[index];
  }
  auto it = extended_register_values_.find(index);
  return it != extended_register_values_.end() ? it->second : 0;
}

void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  RegisterFile& regs = *register_file_;
  if (index >= RegisterFile::kRegisterCount) {
    auto [it, inserted] = extended_register_values_.insert_or_assign(index, value);
    (void)it;
    if (inserted) {
      REXGPU_WARN(
          "CommandProcessor::WriteRegister index out of bounds: {} (stored as extended register)",
          index);
    }
    return;
  }

  // Volatile for the WAIT_REG_MEM loop.
  const_cast<volatile uint32_t&>(regs.values[index]) = value;
  if (!kKnownRegisters[index]) {
    REXGPU_DEBUG("GPU: Write to unknown register ({:04X} = {:08X})", index, value);
  }

  // Scratch register writeback.
  if (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) {
    uint32_t scratch_reg = index - XE_GPU_REG_SCRATCH_REG0;
    if ((1 << scratch_reg) & regs.values[XE_GPU_REG_SCRATCH_UMSK]) {
      // Enabled - write to address.
      uint32_t scratch_addr = regs.values[XE_GPU_REG_SCRATCH_ADDR];
      uint32_t mem_addr = scratch_addr + (scratch_reg * 4);
      memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);
      // The Direct3D code's fences.
      NotifyRingProgress();
    }
  } else {
    switch (index) {
      // If this is a COHER register, set the dirty flag.
      // This will block the command processor the next time it WAIT_REG_MEMs
      // and allow us to synchronize the memory.
      case XE_GPU_REG_COHER_STATUS_HOST: {
        const_cast<volatile uint32_t&>(regs.values[index]) |= UINT32_C(0x80000000);
      } break;

      case XE_GPU_REG_DC_LUT_RW_INDEX: {
        // Reset the sequential read / write component index (see the M56
        // DC_LUT_SEQ_COLOR documentation).
        gamma_ramp_rw_component_ = 0;
      } break;

      case XE_GPU_REG_DC_LUT_SEQ_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // DC_LUT_SEQ_COLOR is in the red, green, blue order, but the write
        // enable mask is blue, green, red.
        bool write_gamma_ramp_component = (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                                           (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          // Bits 0:5 are hardwired to zero.
          uint32_t gamma_ramp_seq_color = regs.Get<reg::DC_LUT_SEQ_COLOR>().seq_color >> 6;
          switch (gamma_ramp_rw_component_) {
            case 0:
              gamma_ramp_entry.color_10_red = gamma_ramp_seq_color;
              break;
            case 1:
              gamma_ramp_entry.color_10_green = gamma_ramp_seq_color;
              break;
            case 2:
              gamma_ramp_entry.color_10_blue = gamma_ramp_seq_color;
              break;
          }
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          ++new_gamma_ramp_rw_index.rw_index;
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                        rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_PWL_DATA: {
        // Should be in the PWL writing mode.
        assert_not_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // Bit 7 of the index is ignored for PWL.
        uint32_t gamma_ramp_rw_index_pwl = gamma_ramp_rw_index.rw_index & 0x7F;
        // DC_LUT_PWL_DATA is likely in the red, green, blue order because
        // DC_LUT_SEQ_COLOR is, but the write enable mask is blue, green, red.
        bool write_gamma_ramp_component = (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                                           (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_PWL_DATA& gamma_ramp_entry =
              gamma_ramp_pwl_rgb_[gamma_ramp_rw_index_pwl][gamma_ramp_rw_component_];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_PWL_DATA>();
          // Bits 0:5 are hardwired to zero.
          gamma_ramp_entry.base = gamma_ramp_value.base & ~UINT32_C(0x3F);
          gamma_ramp_entry.delta = gamma_ramp_value.delta & ~UINT32_C(0x3F);
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          // TODO(Triang3l): Should this increase beyond 7 bits for PWL?
          // Direct3D 9 explicitly sets rw_index to 0x80 after writing the last
          // PWL entry. However, the DC_LUT_RW_INDEX documentation says that for
          // PWL, the bit 7 is ignored.
          new_gamma_ramp_rw_index.rw_index = (gamma_ramp_rw_index.rw_index & ~UINT32_C(0x7F)) |
                                             ((gamma_ramp_rw_index_pwl + 1) & 0x7F);
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                        rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRampPWLValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_30_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        uint32_t gamma_ramp_write_enable_mask = regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & 0b111;
        if (gamma_ramp_write_enable_mask) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_30_COLOR>();
          if (gamma_ramp_write_enable_mask & 0b001) {
            gamma_ramp_entry.color_10_blue = gamma_ramp_value.color_10_blue;
          }
          if (gamma_ramp_write_enable_mask & 0b010) {
            gamma_ramp_entry.color_10_green = gamma_ramp_value.color_10_green;
          }
          if (gamma_ramp_write_enable_mask & 0b100) {
            gamma_ramp_entry.color_10_red = gamma_ramp_value.color_10_red;
          }
        }
        // TODO(Triang3l): Should this reset the component write index? If this
        // increase is assumed to behave like a full DC_LUT_RW_INDEX write, it
        // probably should. Currently this also calls WriteRegister for
        // DC_LUT_RW_INDEX, which resets gamma_ramp_rw_component_ as well.
        gamma_ramp_rw_component_ = 0;
        reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
        ++new_gamma_ramp_rw_index.rw_index;
        WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                      rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        if (gamma_ramp_write_enable_mask) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;
    }
  }
}

void CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = memory::load_and_swap<uint32_t>(base + i);
    WriteRegister(start_index + i, data);
  }
}

void CommandProcessor::WriteRegisterRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  memory::RingBuffer::ReadRange range = ring->BeginRead(size_t(num_registers) * sizeof(uint32_t));
  if (range.first_length != 0) {
    uint32_t first_count = uint32_t(range.first_length / sizeof(uint32_t));
    WriteRegistersFromMem(base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
                          first_count);
    base += first_count;
  }
  if (range.second_length != 0) {
    WriteRegistersFromMem(base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.second)),
                          uint32_t(range.second_length / sizeof(uint32_t)));
  }
  ring->EndRead(range);
}

void CommandProcessor::WriteALURangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                             uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4000, num_registers);
}

void CommandProcessor::WriteFetchRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                               uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4800, num_registers);
}

void CommandProcessor::WriteBoolRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4900, num_registers);
}

void CommandProcessor::WriteLoopRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4908, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                   uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x2000, num_registers);
}

void CommandProcessor::WriteALURangeFromMem(uint32_t start_index, uint32_t* base,
                                            uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4000, base, num_registers);
}

void CommandProcessor::WriteFetchRangeFromMem(uint32_t start_index, uint32_t* base,
                                              uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4800, base, num_registers);
}

void CommandProcessor::WriteBoolRangeFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4900, base, num_registers);
}

void CommandProcessor::WriteLoopRangeFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4908, base, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x2000, base, num_registers);
}

void CommandProcessor::MakeCoherent() {
  SCOPE_profile_cpu_f("gpu");

  // Status host often has 0x01000000 or 0x03000000.
  // This is likely toggling VC (vertex cache) or TC (texture cache).
  // Or, it also has a direction in here maybe - there is probably
  // some way to check for dest coherency (what all the COHER_DEST_BASE_*
  // registers are for).
  // Best docs I've found on this are here:
  // https://web.archive.org/web/20160711162346/https://amd-dev.wpengine.netdna-cdn.com/wordpress/media/2013/10/R6xx_R7xx_3D.pdf
  // https://cgit.freedesktop.org/xorg/driver/xf86-video-radeonhd/tree/src/r6xx_accel.c?id=3f8b6eccd9dba116cc4801e7f80ce21a879c67d2#n454

  // Volatile because this may be called from the WAIT_REG_MEM loop.
  volatile uint32_t* regs_volatile = register_file_->values;
  auto status_host = rex::memory::Reinterpret<reg::COHER_STATUS_HOST>(
      uint32_t(regs_volatile[XE_GPU_REG_COHER_STATUS_HOST]));
  uint32_t base_host = regs_volatile[XE_GPU_REG_COHER_BASE_HOST];
  uint32_t size_host = regs_volatile[XE_GPU_REG_COHER_SIZE_HOST];

  if (!status_host.status) {
    return;
  }

  const char* action = "N/A";
  if (status_host.vc_action_ena && status_host.tc_action_ena) {
    action = "VC | TC";
  } else if (status_host.tc_action_ena) {
    action = "TC";
  } else if (status_host.vc_action_ena) {
    action = "VC";
  }

  // TODO(benvanik): notify resource cache of base->size and type.
  REXGPU_TRACE("Make {:08X} -> {:08X} ({}b) coherent, action = {}", base_host,
               base_host + size_host, size_host, action);

  // Mark coherent.
  regs_volatile[XE_GPU_REG_COHER_STATUS_HOST] = 0;
}

void CommandProcessor::PrepareForWait() {
  trace_writer_.Flush();
}

void CommandProcessor::ReturnFromWait() {}

void CommandProcessor::RecordHostGpuFenceWait(uint64_t host_ticks, bool full_sync) {
  if (REXCVAR_GET(gpu_wait_stats) <= 0) {
    return;
  }
  if (full_sync) {
    g_gpu_full_sync_waits.fetch_add(1, std::memory_order_relaxed);
    g_gpu_full_sync_ticks.fetch_add(host_ticks, std::memory_order_relaxed);
  } else {
    g_gpu_fence_waits.fetch_add(1, std::memory_order_relaxed);
    g_gpu_fence_ticks.fetch_add(host_ticks, std::memory_order_relaxed);
  }
}

uint32_t CommandProcessor::ExecutePrimaryBuffer(uint32_t read_index, uint32_t write_index) {
  SCOPE_profile_cpu_f("gpu");

  // If we have a pending trace stream open it now. That way we ensure we get
  // all commands.
  if (!trace_writer_.is_open() && trace_state_ == TraceState::kStreaming) {
    uint32_t title_id =
        kernel_state_->GetExecutableModule() ? kernel_state_->GetExecutableModule()->title_id() : 0;
    auto file_name = fmt::format("{:08X}_stream.xtr", title_id);
    auto path = trace_stream_path_ / file_name;
    trace_writer_.Open(path, title_id);
    InitializeTrace();
  }

  // Adjust pointer base.
  uint32_t start_ptr = primary_buffer_ptr_ + read_index * sizeof(uint32_t);
  start_ptr = (primary_buffer_ptr_ & ~0x1FFFFFFF) | (start_ptr & 0x1FFFFFFF);
  uint32_t end_ptr = primary_buffer_ptr_ + write_index * sizeof(uint32_t);
  end_ptr = (primary_buffer_ptr_ & ~0x1FFFFFFF) | (end_ptr & 0x1FFFFFFF);

  trace_writer_.WritePrimaryBufferStart(start_ptr, write_index - read_index);

  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(primary_buffer_ptr_), primary_buffer_size_);
  reader.set_read_offset(read_index * sizeof(uint32_t));
  reader.set_write_offset(write_index * sizeof(uint32_t));
  const bool incremental_read_ptr = REXCVAR_GET(gpu_incremental_read_pointer);
  do {
    if (!ExecutePacket(&reader)) {
      // This probably should be fatal - but we're going to continue anyways.
      REXGPU_ERROR("**** PRIMARY RINGBUFFER: Failed to execute packet.");
      assert_always();
      break;
    }
    // Report the progress through the ring after every packet, like the
    // hardware's periodic read pointer writeback, rather than only once the
    // whole batch is done: an indirect buffer's commands have all been parsed
    // when its packet returns, and the game waits for the read pointer before
    // reusing command buffer memory, so it can keep recording instead of
    // spinning until the command processor catches up with everything queued.
    if (incremental_read_ptr && read_ptr_writeback_ptr_) {
      memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_ptr_),
                                       uint32_t(reader.read_offset() / sizeof(uint32_t)));
      NotifyRingProgress();
    }
  } while (reader.read_count());

  OnPrimaryBufferEnd();

  trace_writer_.WritePrimaryBufferEnd();

  return write_index;
}


// PM4 census (diagnostic): what the command stream this game produces is made
// of, as input for replacing it with native rendering calls.
REXCVAR_DEFINE_BOOL(pm4_bulk_float_constants, true, "GPU",
                    "Apply type-0 packets of shader float constants as one range instead of "
                    "register by register");

REXCVAR_DEFINE_INT32(pm4_census, 0, "GPU",
                     "Log a per-frame census of the PM4 command stream every N frames "
                     "(0 = off; diagnostic)");

namespace {
struct Pm4Census {
  uint64_t packets = 0;
  uint64_t dwords = 0;
  uint64_t type_packets[4] = {};
  uint64_t register_writes = 0;
  // Register writes by range: config (< 0x2000), state (0x2000-0x3FFF), vertex
  // float constants, pixel float constants, fetch constants, bool/loop
  // constants, other.
  uint64_t register_range_writes[7] = {};
  uint64_t type3_opcodes[128] = {};
  uint64_t type3_opcode_dwords[128] = {};
  uint64_t indirect_buffers = 0;
  uint64_t indirect_buffer_dwords = 0;
  uint64_t nested_indirect_buffers = 0;
  uint64_t repeated_indirect_buffers = 0;
  // Indirect buffer dwords per 16 MB guest physical region.
  uint64_t region_dwords[32] = {};
  // Indirect buffers with the address and length of one in the previous
  // frame: with the same contents (replays of command buffers recorded once),
  // with different contents (patched in place); and the rest.
  uint64_t patched_indirect_buffers = 0;
  uint64_t new_indirect_buffers = 0;
  uint64_t replay_dwords = 0;
  // Executed inside unchanged replays (including buffers they call).
  uint64_t replay_packets = 0;
  uint64_t replay_register_writes = 0;
  uint64_t replay_draws = 0;
  uint64_t replay_ticks = 0;
  // Of replay_ticks, the time in IssueDraw (turning the draws into host
  // commands) rather than decoding packets.
  uint64_t replay_draw_ticks = 0;
  uint64_t draws = 0;
};
Pm4Census g_pm4_census;
uint32_t g_pm4_census_frames = 0;
uint32_t g_pm4_indirect_depth = 0;
// Indirect buffers of the previous frame: address, length and a hash of their
// first dwords, to spot command buffers replayed unchanged across frames.
std::vector<uint64_t> g_pm4_previous_buffers, g_pm4_current_buffers;
// Indirect buffers by address and length, with a hash of their whole
// contents, this frame and the previous one.
std::unordered_map<uint64_t, uint64_t> g_pm4_previous_ib_contents, g_pm4_current_ib_contents;
// How many unchanged replays the command processor is inside.
uint32_t g_pm4_replay_depth = 0;

uint64_t Pm4BufferKey(const uint32_t* dwords, uint32_t ptr, uint32_t count) {
  uint64_t hash = 1469598103934665603ull;
  for (uint32_t i = 0; i < std::min(count, uint32_t(16)); ++i) {
    hash = (hash ^ dwords[i]) * 1099511628211ull;
  }
  return hash ^ (uint64_t(ptr) << 20) ^ count;
}
}  // namespace

namespace {
// Verify-mode statistics, logged at swaps.
uint64_t g_native_verify_records = 0;
uint64_t g_native_verify_registers = 0;
uint64_t g_native_verify_mismatches = 0;
uint64_t g_native_applied_records = 0;
uint64_t g_native_applied_registers = 0;
uint64_t g_native_missing_records = 0;
uint64_t g_native_fallbacks = 0;
uint32_t g_native_stats_frames = 0;
}  // namespace

void CommandProcessor::ExecuteIndirectBuffer(uint32_t ptr, uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  trace_writer_.WriteIndirectBufferStart(ptr, count * sizeof(uint32_t));

  bool census_replay = false;
  uint64_t census_replay_start_tick = 0;

  if (REXCVAR_GET(pm4_census) > 0) {
    ++g_pm4_census.indirect_buffers;
    g_pm4_census.indirect_buffer_dwords += count;
    g_pm4_census.nested_indirect_buffers += g_pm4_indirect_depth ? 1 : 0;
    g_pm4_census.region_dwords[(ptr >> 24) & 31] += count;
    const uint32_t* ib_dwords = memory_->TranslatePhysical<const uint32_t*>(ptr);
    uint64_t key = Pm4BufferKey(ib_dwords, ptr, count);
    g_pm4_current_buffers.push_back(key);
    if (std::find(g_pm4_previous_buffers.begin(), g_pm4_previous_buffers.end(), key) !=
        g_pm4_previous_buffers.end()) {
      ++g_pm4_census.repeated_indirect_buffers;
    }
    uint64_t location = (uint64_t(ptr) << 20) | count;
    uint64_t contents = XXH3_64bits(ib_dwords, size_t(count) * sizeof(uint32_t));
    auto previous = g_pm4_previous_ib_contents.find(location);
    if (previous == g_pm4_previous_ib_contents.end()) {
      ++g_pm4_census.new_indirect_buffers;
    } else if (previous->second != contents) {
      ++g_pm4_census.patched_indirect_buffers;
    } else {
      census_replay = true;
      g_pm4_census.replay_dwords += count;
      if (!g_pm4_replay_depth) {
        census_replay_start_tick = rex::chrono::Clock::QueryHostTickCount();
      }
      ++g_pm4_replay_depth;
    }
    g_pm4_current_ib_contents[location] = contents;
  }
  ++g_pm4_indirect_depth;

  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      // Return up a level if we encounter a bad packet.
      REXGPU_ERROR("**** INDIRECT RINGBUFFER: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());
  --g_pm4_indirect_depth;
  if (census_replay && !--g_pm4_replay_depth) {
    g_pm4_census.replay_ticks +=
        rex::chrono::Clock::QueryHostTickCount() - census_replay_start_tick;
  }

  trace_writer_.WriteIndirectBufferEnd();
}

void CommandProcessor::ExecutePacket(uint32_t ptr, uint32_t count) {
  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      REXGPU_ERROR("**** ExecutePacket: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());
}

bool CommandProcessor::ExecutePacket(memory::RingBuffer* reader) {
  const uint32_t packet = reader->ReadAndSwap<uint32_t>();
  const uint32_t packet_type = packet >> 30;
  if (REXCVAR_GET(pm4_census) > 0) {
    ++g_pm4_census.packets;
    ++g_pm4_census.type_packets[packet_type];
    uint32_t packet_dwords = 1;
    if (packet_type == 0 || packet_type == 3) {
      packet_dwords += ((packet >> 16) & 0x3FFF) + 1;
    } else if (packet_type == 1) {
      packet_dwords += 2;
    }
    g_pm4_census.dwords += packet_dwords;
    if (g_pm4_replay_depth) {
      ++g_pm4_census.replay_packets;
    }
    if (packet_type == 0) {
      uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
      g_pm4_census.register_writes += count;
      if (g_pm4_replay_depth) {
        g_pm4_census.replay_register_writes += count;
      }
      uint32_t base = packet & 0x7FFF;
      uint32_t range = base < 0x2000   ? 0
                       : base < 0x4000 ? 1
                       : base < 0x4400 ? 2
                       : base < 0x4800 ? 3
                       : base < 0x4900 ? 4
                       : base < 0x4A00 ? 5
                                       : 6;
      g_pm4_census.register_range_writes[range] += count;
    } else if (packet_type == 1) {
      g_pm4_census.register_writes += 2;
      if (g_pm4_replay_depth) {
        g_pm4_census.replay_register_writes += 2;
      }
    } else if (packet_type == 3) {
      uint32_t opcode = (packet >> 8) & 0x7F;
      ++g_pm4_census.type3_opcodes[opcode];
      g_pm4_census.type3_opcode_dwords[opcode] += packet_dwords;
      if (opcode == PM4_DRAW_INDX || opcode == PM4_DRAW_INDX_2) {
        ++g_pm4_census.draws;
        if (g_pm4_replay_depth) {
          ++g_pm4_census.replay_draws;
        }
      }
    }
  }
  if (packet == 0) {
    trace_writer_.WritePacketStart(uint32_t(reader->read_ptr() - 4), 1);
    trace_writer_.WritePacketEnd();
    return true;
  }

  if (packet == 0xCDCDCDCD) {
    REXGPU_WARN("GPU packet is CDCDCDCD - probably read uninitialized memory!");
  }

  switch (packet_type) {
    case 0x00:
      return ExecutePacketType0(reader, packet);
    case 0x01:
      return ExecutePacketType1(reader, packet);
    case 0x02:
      return ExecutePacketType2(reader, packet);
    case 0x03:
      return ExecutePacketType3(reader, packet);
    default:
      assert_unhandled_case(packet_type);
      return false;
  }
}

bool CommandProcessor::ExecutePacketType0(memory::RingBuffer* reader, uint32_t packet) {
  // Type-0 packet.
  // Write count registers in sequence to the registers starting at
  // (base_index << 2).

  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("ExecutePacketType0 overflow (read count {:08X}, packet count {:08X})",
                 reader->read_count(), count * sizeof(uint32_t));
    return false;
  }

  trace_writer_.WritePacketStart(uint32_t(reader->read_ptr() - 4), 1 + count);

  uint32_t base_index = (packet & 0x7FFF);
  uint32_t write_one_reg = (packet >> 15) & 0x1;
  if (!write_one_reg && base_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      base_index + count - 1 <= XE_GPU_REG_SHADER_CONSTANT_511_W &&
      REXCVAR_GET(pm4_bulk_float_constants)) {
    // Most of this game's command stream: no per-register side effects.
    WriteRegisterRangeFromRing(reader, base_index, count);
    trace_writer_.WritePacketEnd();
    return true;
  }
  for (uint32_t m = 0; m < count; m++) {
    uint32_t reg_data = reader->ReadAndSwap<uint32_t>();
    uint32_t target_index = write_one_reg ? base_index : base_index + m;
    WriteRegister(target_index, reg_data);
  }

  trace_writer_.WritePacketEnd();
  return true;
}

bool CommandProcessor::ExecutePacketType1(memory::RingBuffer* reader, uint32_t packet) {
  // Type-1 packet.
  // Contains two registers of data. Type-0 should be more common.
  trace_writer_.WritePacketStart(uint32_t(reader->read_ptr() - 4), 3);
  uint32_t reg_index_1 = packet & 0x7FF;
  uint32_t reg_index_2 = (packet >> 11) & 0x7FF;
  uint32_t reg_data_1 = reader->ReadAndSwap<uint32_t>();
  uint32_t reg_data_2 = reader->ReadAndSwap<uint32_t>();
  WriteRegister(reg_index_1, reg_data_1);
  WriteRegister(reg_index_2, reg_data_2);
  trace_writer_.WritePacketEnd();
  return true;
}

bool CommandProcessor::ExecutePacketType2(memory::RingBuffer* reader, uint32_t packet) {
  // Type-2 packet.
  // No-op. Do nothing.
  trace_writer_.WritePacketStart(uint32_t(reader->read_ptr() - 4), 1);
  trace_writer_.WritePacketEnd();
  return true;
}

bool CommandProcessor::ExecutePacketType3(memory::RingBuffer* reader, uint32_t packet) {
  // Type-3 packet.
  uint32_t opcode = (packet >> 8) & 0x7F;
  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  auto data_start_offset = reader->read_offset();

  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("ExecutePacketType3 overflow (read count {:08X}, packet count {:08X})",
                 reader->read_count(), count * sizeof(uint32_t));
    return false;
  }

  // To handle nesting behavior when tracing we special case indirect buffers.
  if (opcode == PM4_INDIRECT_BUFFER) {
    trace_writer_.WritePacketStart(uint32_t(reader->read_ptr() - 4), 2);
  } else {
    trace_writer_.WritePacketStart(uint32_t(reader->read_ptr() - 4), 1 + count);
  }

  // & 1 == predicate - when set, we do bin check to see if we should execute
  // the packet. Only type 3 packets are affected.
  // We also skip predicated swaps, as they are never valid (probably?).
  if (packet & 1) {
    bool any_pass = (bin_select_ & bin_mask_) != 0;
    if (!any_pass || opcode == PM4_XE_SWAP) {
      reader->AdvanceRead(count * sizeof(uint32_t));
      trace_writer_.WritePacketEnd();
      return true;
    }
  }

  bool result = false;
  switch (opcode) {
    case PM4_ME_INIT:
      result = ExecutePacketType3_ME_INIT(reader, packet, count);
      break;
    case PM4_NOP:
      result = ExecutePacketType3_NOP(reader, packet, count);
      break;
    case PM4_INTERRUPT:
      result = ExecutePacketType3_INTERRUPT(reader, packet, count);
      break;
    case PM4_XE_SWAP:
      result = ExecutePacketType3_XE_SWAP(reader, packet, count);
      break;
    case native_records::kMarkerOpcode:
      result = ExecutePacketType3_NATIVE_RECORD(reader, packet, count);
      break;
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD:
      result = ExecutePacketType3_INDIRECT_BUFFER(reader, packet, count);
      break;
    case PM4_WAIT_REG_MEM:
      result = ExecutePacketType3_WAIT_REG_MEM(reader, packet, count);
      break;
    case PM4_REG_RMW:
      result = ExecutePacketType3_REG_RMW(reader, packet, count);
      break;
    case PM4_REG_TO_MEM:
      result = ExecutePacketType3_REG_TO_MEM(reader, packet, count);
      break;
    case PM4_MEM_WRITE:
      result = ExecutePacketType3_MEM_WRITE(reader, packet, count);
      break;
    case PM4_COND_WRITE:
      result = ExecutePacketType3_COND_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE:
      result = ExecutePacketType3_EVENT_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_SHD:
      result = ExecutePacketType3_EVENT_WRITE_SHD(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_EXT:
      result = ExecutePacketType3_EVENT_WRITE_EXT(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_ZPD:
      result = ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
      break;
    case PM4_DRAW_INDX:
      result = ExecutePacketType3_DRAW_INDX(reader, packet, count);
      break;
    case PM4_DRAW_INDX_2:
      result = ExecutePacketType3_DRAW_INDX_2(reader, packet, count);
      break;
    case PM4_SET_CONSTANT:
      result = ExecutePacketType3_SET_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_CONSTANT2:
      result = ExecutePacketType3_SET_CONSTANT2(reader, packet, count);
      break;
    case PM4_LOAD_ALU_CONSTANT:
      result = ExecutePacketType3_LOAD_ALU_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_SHADER_CONSTANTS:
      result = ExecutePacketType3_SET_SHADER_CONSTANTS(reader, packet, count);
      break;
    case PM4_IM_LOAD:
      result = ExecutePacketType3_IM_LOAD(reader, packet, count);
      break;
    case PM4_IM_LOAD_IMMEDIATE:
      result = ExecutePacketType3_IM_LOAD_IMMEDIATE(reader, packet, count);
      break;
    case PM4_INVALIDATE_STATE:
      result = ExecutePacketType3_INVALIDATE_STATE(reader, packet, count);
      break;
    case PM4_VIZ_QUERY:
      result = ExecutePacketType3_VIZ_QUERY(reader, packet, count);
      break;

    case PM4_SET_BIN_MASK_LO: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_MASK_HI: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_LO: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_HI: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_MASK: {
      assert_true(count == 2);
      uint64_t val_hi = reader->ReadAndSwap<uint32_t>();
      uint64_t val_lo = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (val_hi << 32) | val_lo;
      result = true;
    } break;
    case PM4_SET_BIN_SELECT: {
      assert_true(count == 2);
      uint64_t val_hi = reader->ReadAndSwap<uint32_t>();
      uint64_t val_lo = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (val_hi << 32) | val_lo;
      result = true;
    } break;
    case PM4_CONTEXT_UPDATE: {
      assert_true(count == 1);
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      REXGPU_INFO("GPU context update = {:08X}", value);
      assert_true(value == 0);
      result = true;
      break;
    }
    case PM4_WAIT_FOR_IDLE: {
      // This opcode is used by 5454084E while going / being ingame.
      assert_true(count == 1);
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      REXGPU_INFO("GPU wait for idle = {:08X}", value);
      result = true;
      break;
    }

    default:
      REXGPU_INFO("Unimplemented GPU OPCODE: 0x{:02X}\t\tCOUNT: {}\n", opcode, count);
      assert_always();
      reader->AdvanceRead(count * sizeof(uint32_t));
      break;
  }

  trace_writer_.WritePacketEnd();
  if (opcode == PM4_XE_SWAP) {
    // End the trace writer frame.
    if (trace_writer_.is_open()) {
      trace_writer_.WriteEvent(EventCommand::Type::kSwap);
      trace_writer_.Flush();
      if (trace_state_ == TraceState::kSingleFrame) {
        trace_state_ = TraceState::kDisabled;
        trace_writer_.Close();
      }
    } else if (trace_state_ == TraceState::kSingleFrame) {
      // New trace request - we only start tracing at the beginning of a frame.
      uint32_t title_id = kernel_state_->GetExecutableModule()->title_id();
      auto file_name = fmt::format("{:08X}_{}.xtr", title_id, counter_ - 1);
      auto path = trace_frame_path_ / file_name;
      trace_writer_.Open(path, title_id);
      InitializeTrace();
    }
  }

  assert_true(reader->read_offset() ==
              (data_start_offset + (count * sizeof(uint32_t))) % reader->capacity());
  return result;
}

bool CommandProcessor::ExecutePacketType3_ME_INIT(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  // initialize CP's micro-engine
  me_bin_.clear();
  for (uint32_t i = 0; i < count; i++) {
    me_bin_.push_back(reader->ReadAndSwap<uint32_t>());
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_NOP(memory::RingBuffer* reader, uint32_t packet,
                                              uint32_t count) {
  // skip N 32-bit words to get to the next packet
  // No-op, ignore some data.
  reader->AdvanceRead(count * sizeof(uint32_t));
  return true;
}

bool CommandProcessor::ExecutePacketType3_INTERRUPT(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // generate interrupt from the command stream
  uint32_t cpu_mask = reader->ReadAndSwap<uint32_t>();
  for (int n = 0; n < 6; n++) {
    if (cpu_mask & (1 << n)) {
      if (graphics_system_) {
        graphics_system_->DispatchInterruptCallback(1, n);
      }
    }
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_XE_SWAP(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  {
    static uint64_t last_frame_tick = 0;
    uint64_t now = rex::chrono::Clock::QueryHostTickCount();
    if (last_frame_tick) {
      uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();
      int64_t dt_us = static_cast<int64_t>((now - last_frame_tick) * 1000000 / freq);
      PROFILE_FRAME_TIME_US(dt_us);
      PROFILE_FPS(freq / (now - last_frame_tick));
    }
    last_frame_tick = now;
  }
#endif
  rex::perf::Profiler::Flip();

  // Xenia-specific VdSwap hook.
  // VdSwap will post this to tell us we need to swap the screen/fire an
  // interrupt.
  // 63 words here, but only the first has any data.
  uint32_t magic = reader->ReadAndSwap<memory::fourcc_t>();
  assert_true(magic == kSwapSignature);

  // TODO(benvanik): only swap frontbuffer ptr.
  uint32_t frontbuffer_ptr = reader->ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_width = reader->ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_height = reader->ReadAndSwap<uint32_t>();
  reader->AdvanceRead((count - 4) * sizeof(uint32_t));

  // Pick up changes of the legacy memexport readback cvar once per frame.
  legacy_readback_memexport_overridden_ = -1;

  int32_t wait_stats_interval = REXCVAR_GET(gpu_wait_stats);
  uint64_t swap_start_tick =
      wait_stats_interval > 0 ? rex::chrono::Clock::QueryHostTickCount() : 0;
  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);
  if (wait_stats_interval > 0) {
    uint64_t swap_ticks = rex::chrono::Clock::QueryHostTickCount() - swap_start_tick;
    g_gpu_wait_stats.swap_ticks += swap_ticks;
    g_gpu_wait_stats.max_swap_ticks = std::max(g_gpu_wait_stats.max_swap_ticks, swap_ticks);
    LogGpuWaitStats(uint32_t(wait_stats_interval));
  }

  if ((g_native_verify_records || g_native_applied_records || g_native_missing_records ||
       g_native_fallbacks) &&
      ++g_native_stats_frames >= 300) {
    g_native_stats_frames = 0;
    REXGPU_INFO(
        "[native-records] last 300 frames: applied {} records ({} registers), verified {} "
        "records ({} registers, {} mismatches), {} replays through packets, {} markers "
        "without a record",
        g_native_applied_records, g_native_applied_registers, g_native_verify_records,
        g_native_verify_registers, g_native_verify_mismatches, g_native_fallbacks,
        g_native_missing_records);
    g_native_verify_records = g_native_verify_registers = g_native_verify_mismatches = 0;
    g_native_applied_records = g_native_applied_registers = g_native_missing_records = 0;
    g_native_fallbacks = 0;
  }

  int32_t census_interval = REXCVAR_GET(pm4_census);
  if (census_interval > 0) {
    if (++g_pm4_census_frames >= uint32_t(census_interval)) {
      g_pm4_census_frames = 0;
      const Pm4Census& c = g_pm4_census;
      std::string opcodes;
      std::vector<std::pair<uint64_t, uint32_t>> sorted;
      for (uint32_t i = 0; i < 128; ++i) {
        if (c.type3_opcodes[i]) {
          sorted.emplace_back(c.type3_opcodes[i], i);
        }
      }
      std::sort(sorted.rbegin(), sorted.rend());
      for (const auto& [n, op] : sorted) {
        opcodes += fmt::format(" 0x{:02X}:{}/{}dw", op, n, c.type3_opcode_dwords[op]);
      }
      std::string regions;
      for (uint32_t i = 0; i < 32; ++i) {
        if (c.region_dwords[i]) {
          regions += fmt::format(" {:02X}xxxxxx:{}dw", i, c.region_dwords[i]);
        }
      }
      REXGPU_INFO(
          "[pm4-census] packets {} dwords {} | type0 {} type1 {} type2 {} type3 {} | register "
          "writes {} (config {} state {} vs-float {} ps-float {} fetch {} bool/loop {} other {}) | "
          "indirect buffers {} ({} dw, {} nested, {} same as previous frame) regions{} "
          "| type3 opcodes (count/dwords):{}",
          c.packets, c.dwords, c.type_packets[0], c.type_packets[1], c.type_packets[2],
          c.type_packets[3], c.register_writes, c.register_range_writes[0],
          c.register_range_writes[1], c.register_range_writes[2], c.register_range_writes[3],
          c.register_range_writes[4], c.register_range_writes[5], c.register_range_writes[6],
          c.indirect_buffers, c.indirect_buffer_dwords,
          c.nested_indirect_buffers, c.repeated_indirect_buffers, regions, opcodes);
      // The census covers the one frame it's logged at.
      REXGPU_INFO(
          "[pm4-census] indirect buffers replayed unchanged {} ({} dw), patched {}, new {} | "
          "inside unchanged replays: {} packets, {} register writes, {} of {} draws, {:.2f} ms ({:.2f} "
          "ms of it issuing the draws)",
          c.indirect_buffers - c.patched_indirect_buffers - c.new_indirect_buffers,
          c.replay_dwords, c.patched_indirect_buffers, c.new_indirect_buffers, c.replay_packets,
          c.replay_register_writes, c.replay_draws, c.draws,
          double(c.replay_ticks) * 1000.0 /
              double(rex::chrono::Clock::QueryHostTickFrequency()),
          double(c.replay_draw_ticks) * 1000.0 /
              double(rex::chrono::Clock::QueryHostTickFrequency()));
    }
    g_pm4_census = Pm4Census();
    g_pm4_previous_buffers.swap(g_pm4_current_buffers);
    g_pm4_current_buffers.clear();
    g_pm4_previous_ib_contents.swap(g_pm4_current_ib_contents);
    g_pm4_current_ib_contents.clear();
  }

  ++counter_;
  return true;
}


bool CommandProcessor::ExecutePacketType3_NATIVE_RECORD(memory::RingBuffer* reader,
                                                        uint32_t packet, uint32_t count) {
  uint32_t sequence = reader->ReadAndSwap<uint32_t>();
  uint32_t replaced_dwords = count > 1 ? reader->ReadAndSwap<uint32_t>() : 0;
  if (count > 2) {
    reader->AdvanceRead((count - 2) * sizeof(uint32_t));
  }
  native_records::Record record;
  if (replaced_dwords && reader->read_count() < replaced_dwords * sizeof(uint32_t)) {
    // The packets are not all in this buffer; execute what is there.
    ++g_native_fallbacks;
    return true;
  }
  if (!native_records::Pop(sequence, record)) {
    if (replaced_dwords) {
      // Replayed recorded command buffer: its packets follow and execute.
      ++g_native_fallbacks;
      return true;
    }
    // In a trace replay the record's registers arrive as a register command
    // instead. Live, this would be a lost record.
    if (g_native_missing_records < 12) {
      REXGPU_WARN(
          "[native-records] marker {} without its record (oldest queued {}), at guest {:08X}, "
          "indirect buffer depth {}",
          sequence, native_records::OldestSequence(),
          uint32_t(reader->read_ptr() - reinterpret_cast<uintptr_t>(memory_->TranslatePhysical(0))) - 8,
          g_pm4_indirect_depth);
    }
    ++g_native_missing_records;
    return true;
  }
  switch (record.type) {
    case native_records::RecordType::kRegisterRuns: {
      bool verify = (record.flags & native_records::kRecordFlagVerify) != 0;
      const uint32_t* run = record.payload.data();
      const uint32_t* end = run + record.payload.size();
      while (end - run >= 2) {
        uint32_t first_register = run[0];
        uint32_t register_count = run[1];
        run += 2;
        if (uint32_t(end - run) < register_count ||
            first_register + register_count > RegisterFile::kRegisterCount) {
          break;
        }
        if (verify) {
          for (uint32_t i = 0; i < register_count; ++i) {
            uint32_t expected = rex::byte_swap(run[i]);
            if (register_file_->values[first_register + i] != expected) {
              if (g_native_verify_mismatches < 8) {
                REXGPU_WARN(
                    "[native-records] verify mismatch: register {:04X} record {:08X} packets "
                    "{:08X}",
                    first_register + i, expected, register_file_->values[first_register + i]);
              }
              ++g_native_verify_mismatches;
            }
          }
          g_native_verify_registers += register_count;
        } else {
          WriteRegistersFromMem(first_register, const_cast<uint32_t*>(run), register_count);
          if (trace_writer_.is_open()) {
            trace_writer_.WriteRegisters(first_register, register_file_->values + first_register,
                                         register_count, false);
          }
          g_native_applied_registers += register_count;
        }
        run += register_count;
      }
      if (verify) {
        ++g_native_verify_records;
      } else {
        ++g_native_applied_records;
      }
      if (replaced_dwords) {
        reader->AdvanceRead(replaced_dwords * sizeof(uint32_t));
      }
      break;
    }
    default:
      REXGPU_ERROR("[native-records] unknown record type {}", uint32_t(record.type));
      break;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_INDIRECT_BUFFER(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // indirect buffer dispatch
  uint32_t list_ptr = CpuToGpu(reader->ReadAndSwap<uint32_t>());
  uint32_t list_length = reader->ReadAndSwap<uint32_t>();
  assert_zero(list_length & ~0xFFFFF);
  list_length &= 0xFFFFF;
  ExecuteIndirectBuffer(GpuToCpu(list_ptr), list_length);
  return true;
}

bool CommandProcessor::ExecutePacketType3_WAIT_REG_MEM(memory::RingBuffer* reader, uint32_t packet,
                                                       uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // wait until a register or memory location is a specific value

  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t wait = reader->ReadAndSwap<uint32_t>();

  bool is_memory = (wait_info & 0x10) != 0;

  const bool wait_stats = REXCVAR_GET(gpu_wait_stats) > 0;
  uint64_t unmet_start_tick = 0;
  uint32_t unmet_value = 0;
  bool matched = false;
  do {
    uint32_t value = 0;
    if (is_memory) {
      value =
          *reinterpret_cast<uint32_t*>(memory_->TranslatePhysical(poll_reg_addr & ~uint32_t(0x3)));
      trace_writer_.WriteMemoryRead(CpuToGpu(poll_reg_addr & ~uint32_t(0x3)), sizeof(uint32_t));
      value = xenos::GpuSwap(value, static_cast<xenos::Endian>(poll_reg_addr & 0x3));
    } else {
      value = ReadRegisterValue(poll_reg_addr);
      if (poll_reg_addr == XE_GPU_REG_COHER_STATUS_HOST) {
        MakeCoherent();
        value = ReadRegisterValue(poll_reg_addr);
      }
    }
    switch (wait_info & 0x7) {
      case 0x0:  // Never.
        matched = false;
        break;
      case 0x1:  // Less than reference.
        matched = (value & mask) < ref;
        break;
      case 0x2:  // Less than or equal to reference.
        matched = (value & mask) <= ref;
        break;
      case 0x3:  // Equal to reference.
        matched = (value & mask) == ref;
        break;
      case 0x4:  // Not equal to reference.
        matched = (value & mask) != ref;
        break;
      case 0x5:  // Greater than or equal to reference.
        matched = (value & mask) >= ref;
        break;
      case 0x6:  // Greater than reference.
        matched = (value & mask) > ref;
        break;
      case 0x7:  // Always
        matched = true;
        break;
    }
    if (!matched) {
      if (wait_stats && !unmet_start_tick) {
        unmet_start_tick = rex::chrono::Clock::QueryHostTickCount();
        unmet_value = value;
      }
      // Wait.
      if (wait >= 0x100) {
        if (wait_stats) {
          ++g_gpu_wait_stats.wait_sleeps;
        }
        PrepareForWait();
        if (!REXCVAR_GET(vsync)) {
          // User wants it fast and dangerous.
          rex::thread::MaybeYield();
        } else {
          // HAND PATCH: `wait` is the guest's suggested poll interval, not a
          // required duration (real hardware just re-polls). Sleeping the full
          // interval (e.g. 4 ms at the common 0x400) meant the condition was
          // noticed up to that long after it became true, serializing the ring
          // behind a stall that had already ended. Poll at least every 500 us
          // instead; still >95% idle, but the wake-up lag stops being a
          // per-frame tax.
          uint64_t wait_us = uint64_t(wait) * 1000 / 0x100;
          rex::thread::Sleep(std::chrono::microseconds(std::min<uint64_t>(wait_us, 500)));
        }
        rex::thread::SyncMemory();
        ReturnFromWait();

        if (!worker_running_) {
          // Short-circuited exit.
          return false;
        }
      } else {
        rex::thread::MaybeYield();
      }
    }
  } while (!matched);

  if (wait_stats) {
    RecordGpuWait(poll_reg_addr, wait_info, mask, ref, wait, g_pm4_indirect_depth != 0,
                  unmet_start_tick != 0, unmet_value,
                  unmet_start_tick ? rex::chrono::Clock::QueryHostTickCount() - unmet_start_tick
                                   : 0);
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_RMW(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  // register read/modify/write
  // ? (used during shader upload and edram setup)
  uint32_t rmw_info = reader->ReadAndSwap<uint32_t>();
  uint32_t and_mask = reader->ReadAndSwap<uint32_t>();
  uint32_t or_mask = reader->ReadAndSwap<uint32_t>();
  uint32_t value = register_file_->values[rmw_info & 0x1FFF];
  if ((rmw_info >> 31) & 0x1) {
    // & reg
    value &= register_file_->values[and_mask & 0x1FFF];
  } else {
    // & imm
    value &= and_mask;
  }
  if ((rmw_info >> 30) & 0x1) {
    // | reg
    value |= register_file_->values[or_mask & 0x1FFF];
  } else {
    // | imm
    value |= or_mask;
  }
  WriteRegister(rmw_info & 0x1FFF, value);
  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_TO_MEM(memory::RingBuffer* reader, uint32_t packet,
                                                     uint32_t count) {
  // Copy Register to Memory (?)
  // Count is 2, assuming a Register Addr and a Memory Addr.

  uint32_t reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t mem_addr = reader->ReadAndSwap<uint32_t>();

  uint32_t reg_val = ReadRegisterValue(reg_addr);

  auto endianness = static_cast<xenos::Endian>(mem_addr & 0x3);
  mem_addr &= ~0x3;
  reg_val = GpuSwap(reg_val, endianness);
  memory::store(memory_->TranslatePhysical(mem_addr), reg_val);
  trace_writer_.WriteMemoryWrite(CpuToGpu(mem_addr), 4);

  return true;
}

bool CommandProcessor::ExecutePacketType3_MEM_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  uint32_t write_addr = reader->ReadAndSwap<uint32_t>();
  for (uint32_t i = 0; i < count - 1; i++) {
    uint32_t write_data = reader->ReadAndSwap<uint32_t>();

    auto endianness = static_cast<xenos::Endian>(write_addr & 0x3);
    auto addr = write_addr & ~0x3;
    write_data = GpuSwap(write_data, endianness);
    memory::store(memory_->TranslatePhysical(addr), write_data);
    trace_writer_.WriteMemoryWrite(CpuToGpu(addr), 4);
    write_addr += 4;
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_COND_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                     uint32_t count) {
  // conditional write to memory or register
  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t write_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t write_data = reader->ReadAndSwap<uint32_t>();
  uint32_t value;
  if (wait_info & 0x10) {
    // Memory.
    auto endianness = static_cast<xenos::Endian>(poll_reg_addr & 0x3);
    poll_reg_addr &= ~0x3;
    trace_writer_.WriteMemoryRead(CpuToGpu(poll_reg_addr), 4);
    value = memory::load<uint32_t>(memory_->TranslatePhysical(poll_reg_addr));
    value = GpuSwap(value, endianness);
  } else {
    // Register.
    value = ReadRegisterValue(poll_reg_addr);
  }
  bool matched = false;
  switch (wait_info & 0x7) {
    case 0x0:  // Never.
      matched = false;
      break;
    case 0x1:  // Less than reference.
      matched = (value & mask) < ref;
      break;
    case 0x2:  // Less than or equal to reference.
      matched = (value & mask) <= ref;
      break;
    case 0x3:  // Equal to reference.
      matched = (value & mask) == ref;
      break;
    case 0x4:  // Not equal to reference.
      matched = (value & mask) != ref;
      break;
    case 0x5:  // Greater than or equal to reference.
      matched = (value & mask) >= ref;
      break;
    case 0x6:  // Greater than reference.
      matched = (value & mask) > ref;
      break;
    case 0x7:  // Always
      matched = true;
      break;
  }
  if (matched) {
    // Write.
    if (wait_info & 0x100) {
      // Memory.
      auto endianness = static_cast<xenos::Endian>(write_reg_addr & 0x3);
      write_reg_addr &= ~0x3;
      write_data = GpuSwap(write_data, endianness);
      memory::store(memory_->TranslatePhysical(write_reg_addr), write_data);
      trace_writer_.WriteMemoryWrite(CpuToGpu(write_reg_addr), 4);
    } else {
      // Register.
      WriteRegister(write_reg_addr, write_data);
    }
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                      uint32_t count) {
  // generate an event that creates a write to memory when completed
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  if (count == 1) {
    // Just an event flag? Where does this write?
  } else {
    // Write to an address.
    assert_always();
    reader->AdvanceRead((count - 1) * sizeof(uint32_t));
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_SHD(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // generate a VS|PS_done event
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  uint32_t value = reader->ReadAndSwap<uint32_t>();

  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  uint32_t data_value;
  if ((initiator >> 31) & 0x1) {
    // Write counter (GPU vblank counter?).
    data_value = counter_;
  } else {
    // Write value.
    data_value = value;
  }
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;
  data_value = GpuSwap(data_value, endianness);
  memory::store(memory_->TranslatePhysical(address), data_value);
  trace_writer_.WriteMemoryWrite(CpuToGpu(address), 4);
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_EXT(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // generate a screen extent event
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;

  // Let us hope we can fake this.
  // This callback tells the driver the xy coordinates affected by a previous
  // drawcall.
  // https://www.google.com/patents/US20060055701
  uint16_t extents[] = {
      0 >> 3,                                    // min x
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max x
      0 >> 3,                                    // min y
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max y
      0,                                         // min z
      1,                                         // max z
  };
  assert_true(endianness == xenos::Endian::k8in16);
  memory::copy_and_swap_16_unaligned(memory_->TranslatePhysical(address), extents,
                                     rex::countof(extents));
  trace_writer_.WriteMemoryWrite(CpuToGpu(address), sizeof(extents));
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // Set by D3D as BE but struct ABI is LE
  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  // Occlusion queries:
  // This command is send on query begin and end.
  // As a workaround report some fixed amount of passed samples.
  auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);
  if (fake_sample_count >= 0) {
    auto* pSampleCounts = memory_->TranslatePhysical<xe_gpu_depth_sample_counts*>(
        register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR]);
    if (!pSampleCounts) {
      return true;
    }
    // 0xFFFFFEED is written to this two locations by D3D only on D3DISSUE_END
    // and used to detect a finished query.
    bool is_end_via_z_pass =
        pSampleCounts->ZPass_A == kQueryFinished && pSampleCounts->ZPass_B == kQueryFinished;
    // Older versions of D3D also checks for ZFail (4D5307D5).
    bool is_end_via_z_fail =
        pSampleCounts->ZFail_A == kQueryFinished && pSampleCounts->ZFail_B == kQueryFinished;
    std::memset(pSampleCounts, 0, sizeof(xe_gpu_depth_sample_counts));
    if (is_end_via_z_pass || is_end_via_z_fail) {
      pSampleCounts->ZPass_A = fake_sample_count;
      pSampleCounts->Total_A = fake_sample_count;
    }
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3Draw(memory::RingBuffer* reader, uint32_t packet,
                                              const char* opcode_name, uint32_t viz_query_condition,
                                              uint32_t count_remaining) {
  // if viz_query_condition != 0, this is a conditional draw based on viz query.
  // This ID matches the one issued in PM4_VIZ_QUERY
  // uint32_t viz_id = viz_query_condition & 0x3F;
  // when true, render conditionally based on query result
  // uint32_t viz_use = viz_query_condition & 0x100;

  assert_not_zero(count_remaining);
  if (!count_remaining) {
    REXGPU_ERROR("{}: Packet too small, can't read VGT_DRAW_INITIATOR", opcode_name);
    return false;
  }
  reg::VGT_DRAW_INITIATOR vgt_draw_initiator;
  vgt_draw_initiator.value = reader->ReadAndSwap<uint32_t>();
  --count_remaining;
  WriteRegister(XE_GPU_REG_VGT_DRAW_INITIATOR, vgt_draw_initiator.value);

  bool draw_succeeded = true;
  // TODO(Triang3l): Remove IndexBufferInfo and replace handling of all this
  // with PrimitiveProcessor when the old Vulkan renderer is removed.
  bool is_indexed = false;
  IndexBufferInfo index_buffer_info;
  switch (vgt_draw_initiator.source_select) {
    case xenos::SourceSelect::kDMA: {
      // Indexed draw.
      is_indexed = true;

      // Two separate bounds checks so if there's only one missing register
      // value out of two, one uint32_t will be skipped in the command buffer,
      // not two.
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        REXGPU_ERROR("{}: Packet too small, can't read VGT_DMA_BASE", opcode_name);
        return false;
      }
      uint32_t vgt_dma_base = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      WriteRegister(XE_GPU_REG_VGT_DMA_BASE, vgt_dma_base);
      reg::VGT_DMA_SIZE vgt_dma_size;
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        REXGPU_ERROR("{}: Packet too small, can't read VGT_DMA_SIZE", opcode_name);
        return false;
      }
      vgt_dma_size.value = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      WriteRegister(XE_GPU_REG_VGT_DMA_SIZE, vgt_dma_size.value);

      uint32_t index_size_bytes = vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16
                                      ? sizeof(uint16_t)
                                      : sizeof(uint32_t);
      // The base address must already be word-aligned according to the R6xx
      // documentation, but for safety.
      index_buffer_info.guest_base = vgt_dma_base & ~(index_size_bytes - 1);
      index_buffer_info.endianness = vgt_dma_size.swap_mode;
      index_buffer_info.format = vgt_draw_initiator.index_size;
      index_buffer_info.length = size_t(vgt_dma_size.num_words) * index_size_bytes;
      index_buffer_info.count = vgt_draw_initiator.num_indices;
    } break;
    case xenos::SourceSelect::kImmediate: {
      // TODO(Triang3l): VGT_IMMED_DATA.
      REXGPU_ERROR(
          "{}: Using immediate vertex indices, which are not supported yet. "
          "Report the game to Xenia developers!",
          opcode_name, uint32_t(vgt_draw_initiator.source_select));
      draw_succeeded = false;
      assert_always();
    } break;
    case xenos::SourceSelect::kAutoIndex: {
      // Auto draw.
      index_buffer_info.guest_base = 0;
      index_buffer_info.length = 0;
    } break;
    default: {
      // Invalid source selection.
      draw_succeeded = false;
      assert_unhandled_case(vgt_draw_initiator.source_select);
    } break;
  }

  // Skip to the next command, for example, if there are immediate indexes that
  // we don't support yet.
  reader->AdvanceRead(count_remaining * sizeof(uint32_t));

  if (draw_succeeded) {
    auto viz_query = register_file_->Get<reg::PA_SC_VIZ_QUERY>();
    if (!(viz_query.viz_query_ena && viz_query.kill_pix_post_hi_z)) {
      // TODO(Triang3l): Don't drop the draw call completely if the vertex
      // shader has memexport.
      // TODO(Triang3l || JoelLinn): Handle this properly in the render
      // backends.

      bool major_mode_explicit =
          xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, vgt_draw_initiator.prim_type);
      uint64_t census_draw_start_tick =
          g_pm4_replay_depth ? rex::chrono::Clock::QueryHostTickCount() : 0;
      draw_succeeded = IssueDraw(vgt_draw_initiator.prim_type, vgt_draw_initiator.num_indices,
                                 is_indexed ? &index_buffer_info : nullptr, major_mode_explicit);
      if (g_pm4_replay_depth) {
        g_pm4_census.replay_draw_ticks +=
            rex::chrono::Clock::QueryHostTickCount() - census_draw_start_tick;
      }
      if (!draw_succeeded) {
        // HAND PATCH: rate-limit this log. This game rejects hundreds of draws
        // per frame while streaming (invalid vertex fetch constants during
        // entity load), producing tens of thousands of identical lines in
        // seconds -- enough I/O to rotate away the useful log context.
        static std::atomic<uint64_t> draw_failed_count{0};
        uint64_t n = draw_failed_count.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n & 1023) == 0) {
          auto vgt_output_path_cntl = register_file_->Get<reg::VGT_OUTPUT_PATH_CNTL>();
          auto vgt_hos_cntl = register_file_->Get<reg::VGT_HOS_CNTL>();
          auto rb_modecontrol = register_file_->Get<reg::RB_MODECONTROL>();
          REXGPU_ERROR(
              "{}({}, {}, {}): Failed in backend "
              "(major_mode={}, explicit_major={}, path_select={}, tess_mode={}, edram_mode={}) "
              "[occurrence {}; rate-limited]",
              opcode_name, static_cast<uint32_t>(vgt_draw_initiator.num_indices),
              uint32_t(vgt_draw_initiator.prim_type), uint32_t(vgt_draw_initiator.source_select),
              uint32_t(vgt_draw_initiator.major_mode), uint32_t(major_mode_explicit),
              uint32_t(vgt_output_path_cntl.path_select), uint32_t(vgt_hos_cntl.tess_mode),
              uint32_t(rb_modecontrol.edram_mode), n + 1);
        }
      }
    }
  }

  // If read the packed correctly, but merely couldn't execute it (because of,
  // for instance, features not supported by the host), don't terminate command
  // buffer processing as that would leave rendering in a way more inconsistent
  // state than just a single dropped draw command.
  return true;
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  // "initiate fetch of index buffer and draw"
  // Generally used by Xbox 360 Direct3D 9 for kDMA and kAutoIndex sources.
  // With a viz query token as the first one.
  uint32_t count_remaining = count;
  assert_not_zero(count_remaining);
  if (!count_remaining) {
    REXGPU_ERROR("PM4_DRAW_INDX: Packet too small, can't read the viz query token");
    return false;
  }
  uint32_t viz_query_condition = reader->ReadAndSwap<uint32_t>();
  --count_remaining;
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX", viz_query_condition,
                                count_remaining);
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX_2(memory::RingBuffer* reader, uint32_t packet,
                                                      uint32_t count) {
  // "draw using supplied indices in packet"
  // Generally used by Xbox 360 Direct3D 9 for kAutoIndex source.
  // No viz query token.
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX_2", 0, count);
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT(memory::RingBuffer* reader, uint32_t packet,
                                                       uint32_t count) {
  // load constant into chip and to memory
  // PM4_REG(reg) ((0x4 << 16) | (GSL_HAL_SUBBLOCK_OFFSET(reg)))
  //                                     reg - 0x2000
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t count_registers = count - 1;
  switch (type) {
    case 0:  // ALU
      WriteALURangeFromRing(reader, index, count_registers);
      break;
    case 1:  // FETCH
      WriteFetchRangeFromRing(reader, index, count_registers);
      break;
    case 2:  // BOOL
      WriteBoolRangeFromRing(reader, index, count_registers);
      break;
    case 3:  // LOOP
      WriteLoopRangeFromRing(reader, index, count_registers);
      break;
    case 4:  // REGISTERS
      WriteREGISTERSRangeFromRing(reader, index, count_registers);
      break;
    default:
      assert_always();
      reader->AdvanceRead((count - 1) * sizeof(uint32_t));
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT2(memory::RingBuffer* reader, uint32_t packet,
                                                        uint32_t count) {
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_LOAD_ALU_CONSTANT(memory::RingBuffer* reader,
                                                            uint32_t packet, uint32_t count) {
  // load constants from memory
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  address &= 0x3FFFFFFF;
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t size_dwords = reader->ReadAndSwap<uint32_t>();
  size_dwords &= 0xFFF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t* xlat_address = memory_->TranslatePhysical<uint32_t*>(address);
  switch (type) {
    case 0:  // ALU
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);
      WriteALURangeFromMem(index, xlat_address, size_dwords);
      break;
    case 1:  // FETCH
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);
      WriteFetchRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 2:  // BOOL
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);
      WriteBoolRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 3:  // LOOP
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);
      WriteLoopRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 4:  // REGISTERS
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);
      WriteREGISTERSRangeFromMem(index, xlat_address, size_dwords);
      break;
    default:
      assert_always();
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_SHADER_CONSTANTS(memory::RingBuffer* reader,
                                                               uint32_t packet, uint32_t count) {
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (pointer-based)
  uint32_t addr_type = reader->ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(addr_type & 0x3);
  uint32_t addr = addr_type & ~0x3;
  uint32_t start_size = reader->ReadAndSwap<uint32_t>();
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);

  trace_writer_.WriteMemoryRead(CpuToGpu(addr), size_dwords * 4);
  auto shader =
      LoadShader(shader_type, addr, memory_->TranslatePhysical<uint32_t*>(addr), size_dwords);
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      assert_unhandled_case(shader_type);
      return false;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD_IMMEDIATE(memory::RingBuffer* reader,
                                                            uint32_t packet, uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (code embedded in packet)
  uint32_t dword0 = reader->ReadAndSwap<uint32_t>();
  uint32_t dword1 = reader->ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(dword0);
  uint32_t start_size = dword1;
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);
  assert_true(reader->read_count() >= size_dwords * 4);
  assert_true(count - 2 >= size_dwords);
  auto shader = LoadShader(shader_type, uint32_t(reader->read_ptr()),
                           reinterpret_cast<uint32_t*>(reader->read_ptr()), size_dwords);
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      assert_unhandled_case(shader_type);
      return false;
  }
  reader->AdvanceRead(size_dwords * sizeof(uint32_t));
  return true;
}

bool CommandProcessor::ExecutePacketType3_INVALIDATE_STATE(memory::RingBuffer* reader,
                                                           uint32_t packet, uint32_t count) {
  // selective invalidation of state pointers
  /*uint32_t mask =*/reader->ReadAndSwap<uint32_t>();
  // driver_->InvalidateState(mask);
  return true;
}

bool CommandProcessor::ExecutePacketType3_VIZ_QUERY(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  // begin/end initiator for viz query extent processing
  // https://www.google.com/patents/US20050195186
  assert_true(count == 1);

  uint32_t dword0 = reader->ReadAndSwap<uint32_t>();

  uint32_t id = dword0 & 0x3F;
  uint32_t end = dword0 & 0x100;
  if (!end) {
    // begin a new viz query @ id
    // On hardware this clears the internal state of the scan converter (which
    // is different to the register)
    WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, VIZQUERY_START);
    REXGPU_INFO("Begin viz query ID {:02X}", id);
  } else {
    // end the viz query
    WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, VIZQUERY_END);
    REXGPU_INFO("End viz query ID {:02X}", id);
    // The scan converter writes the internal result back to the register here.
    // We just fake it and say it was visible in case it is read back.
    if (id < 32) {
      register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0] |= uint32_t(1) << id;
    } else {
      register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1] |= uint32_t(1) << (id - 32);
    }
  }

  return true;
}

void CommandProcessor::InitializeTrace() {
  // Write the initial register values, to be loaded directly into the
  // RegisterFile since all registers, including those that may have side
  // effects on setting, will be saved.
  trace_writer_.WriteRegisters(0, register_file_->values, RegisterFile::kRegisterCount, false);

  trace_writer_.WriteGammaRamp(gamma_ramp_256_entry_table(), gamma_ramp_pwl_rgb(),
                               gamma_ramp_rw_component_);
}

}  // namespace rex::graphics
