// Draw-submission telemetry: which guest function issues the draw storm.
//
// The engine already counts host draw calls per frame (the main menu shows
// ~3,700 of ~3 vertices each), but that number says nothing about WHO asked
// for them. This file answers that, and it is the input to the native UI
// batcher: you cannot hook the routine responsible until you know which one
// it is.
//
// Every guest function that composes a PM4 draw header was found by scanning
// the recompiled image for type-3 headers (0xC0______ with opcode byte 0x22
// DRAW_INDX or 0x36 DRAW_INDX_2), built either as lis+ori or oris+ori. That
// scan finds more emitters than the D3D method table lists, and it also
// clears one the table wrongly accuses: entry 45 (sub_82464D98) composes no
// draw header at all -- its 0x2200 is a `li r5,8704` argument to the generic
// type-0 register-packet writer, i.e. a register base index. It is
// deliberately NOT hooked here.
//
// Two things make this actually answer the question, both learned the hard
// way from the previous version:
//
//  1. CALLER ATTRIBUTION. Most of these emitters have no direct call sites at
//     all -- they are reached through the D3D device vtable. So a per-hook
//     count only ever says "the draw routine ran 3,700 times", which is what
//     we already knew. Recording the link register at entry says who called
//     it. Raw addresses are logged; resolve them offline against the
//     function table (tools/codegraph, or simpsons_init.cpp's PPCFuncMappings).
//
//  2. PER-FRAME, NOT CUMULATIVE. "3,700 draws in one frame" and "3,700 draws
//     spread over a minute" are completely different findings, and the old
//     cumulative-since-boot counters could not tell them apart. Everything
//     here is bucketed by the guest frame index, which VdSwap stamps on the
//     guest thread at the true frame boundary.
//
// With native_gpu_calls disabled, calls forward to the original body with
// untouched registers. The experimental native producers handle the bounded
// writers and prepared BeginVertices when enabled; telemetry records both routes.
//
// Argument decode is per-hook and deliberately conservative. (prim = r4 & 0x3F,
// count = r5 & 0xFFFF) was verified by reading the decode in each body; where
// it was not verified the hook counts calls only, because a wrong decode does
// not produce an obviously wrong number -- it produces a plausible one.

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/frame_index.h>
#include <rex/ppc.h>

#include "native_gpu_calls.h"

REXCVAR_DEFINE_BOOL(draw_telemetry, false, "GPU",
                    "Log per-frame draw-call attribution (which guest function issues "
                    "draws, and who calls it). Diagnostic; leave off for normal play.");

// Verified draw-header emitters. Ordering matches kHooks below.
REX_EXTERN(__imp__sub_82462810);
REX_EXTERN(__imp__sub_824646E8);
REX_EXTERN(__imp__sub_824626B8);
REX_EXTERN(__imp__sub_82463E60);
REX_EXTERN(__imp__sub_82464590);
REX_EXTERN(__imp__sub_82455570);
REX_EXTERN(__imp__sub_82444A48);
REX_EXTERN(__imp__sub_82444A80);
REX_EXTERN(__imp__sub_8244C450);
REX_EXTERN(__imp__sub_8244C970);
REX_EXTERN(__imp__sub_8244CF78);
REX_EXTERN(__imp__sub_8244D360);
REX_EXTERN(__imp__sub_8244D7B8);
REX_EXTERN(__imp__sub_8244DC30);
REX_EXTERN(__imp__sub_824529C0);
REX_EXTERN(__imp__sub_8246AEF0);
REX_EXTERN(__imp__sub_8246AF58);
REX_EXTERN(__imp__sub_8246C750);

REX_EXTERN(sub_82462810);
REX_EXTERN(sub_824646E8);
REX_EXTERN(sub_824626B8);
REX_EXTERN(sub_82463E60);
REX_EXTERN(sub_82464590);
REX_EXTERN(sub_82455570);
REX_EXTERN(sub_82444A48);
REX_EXTERN(sub_82444A80);
REX_EXTERN(sub_8244C450);
REX_EXTERN(sub_8244C970);
REX_EXTERN(sub_8244CF78);
REX_EXTERN(sub_8244D360);
REX_EXTERN(sub_8244D7B8);
REX_EXTERN(sub_8244DC30);
REX_EXTERN(sub_824529C0);
REX_EXTERN(sub_8246AEF0);
REX_EXTERN(sub_8246AF58);
REX_EXTERN(sub_8246C750);

namespace {

constexpr size_t kHookCount = 18;

struct HookInfo {
  const char* name;
  // Whether (prim = r4 & 0x3F, count = r5 & 0xFFFF) was verified by reading
  // this function's own decode. Unverified hooks count calls only.
  bool decode_args;
  // Emitters that only run during init/teardown. A nonzero count here during
  // steady-state play is itself the finding.
  bool init_time;
};

constexpr std::array<HookInfo, kHookCount> kHooks{{
    {"82462810", true, false},   // method table entry 18
    {"824646E8", true, false},   // entry 39
    {"824626B8", true, false},   // entry 17
    {"82463E60", false, false},  // entry 33, constant initiator; reads neither r4 nor r5
    {"82464590", true, false},   // entry 38
    {"82455570", false, false},  // r5 is a pointer here, not a count
    {"82444A48", false, true},   // separate submission path, init-time
    {"82444A80", false, true},   // separate submission path, init-time
    {"8244C450", true, false},   // 33 direct call sites; strongest storm candidate
    {"8244C970", false, false},
    {"8244CF78", false, false},
    {"8244D360", false, false},  // 16 direct call sites
    {"8244D7B8", false, false},
    {"8244DC30", false, false},
    {"824529C0", false, false},
    {"8246AEF0", false, false},  // no direct call sites: vtable-reached
    {"8246AF58", false, false},  // no direct call sites: vtable-reached
    {"8246C750", false, false},  // no direct call sites: vtable-reached
}};

// Top-N distinct callers tracked per hook. Small and fixed: this runs on the
// guest render thread in the middle of a draw storm, so it must not allocate.
constexpr size_t kCallerSlots = 8;

struct CallerSlot {
  uint32_t lr = 0;
  uint32_t count = 0;
};

struct HookFrameStats {
  uint32_t calls = 0;
  // Index-count histogram: <=4, <=16, <=64, <=256, <=1024, >1024.
  std::array<uint32_t, 6> count_buckets{};
  // Primitive types seen, as a bitmask over the 6-bit type space.
  uint64_t prim_mask = 0;
  std::array<CallerSlot, kCallerSlots> callers{};
  uint32_t callers_overflowed = 0;
};

// Single-threaded by construction: the game submits draws from one render
// thread. Reads of the frame index are atomic; the stats themselves are not,
// which is the point -- this must be cheap enough not to distort what it
// measures.
HookFrameStats g_stats[kHookCount];
uint64_t g_frame = UINT64_MAX;
bool g_enabled_cached = false;
bool g_enable_checked = false;

size_t CountBucket(uint32_t count) {
  if (count <= 4) return 0;
  if (count <= 16) return 1;
  if (count <= 64) return 2;
  if (count <= 256) return 3;
  if (count <= 1024) return 4;
  return 5;
}

void RecordCaller(HookFrameStats& stats, uint32_t lr) {
  for (auto& slot : stats.callers) {
    if (slot.count == 0) {
      slot.lr = lr;
      slot.count = 1;
      return;
    }
    if (slot.lr == lr) {
      ++slot.count;
      return;
    }
  }
  ++stats.callers_overflowed;
}

void ReportFrame(uint64_t frame) {
  uint32_t total = 0;
  for (const auto& stats : g_stats) {
    total += stats.calls;
  }
  if (!total) {
    return;
  }
  for (size_t h = 0; h < kHookCount; ++h) {
    const HookFrameStats& stats = g_stats[h];
    if (!stats.calls) {
      continue;
    }
    std::string callers;
    for (const auto& slot : stats.callers) {
      if (!slot.count) {
        break;
      }
      callers += fmt::format(" {:08X}x{}", slot.lr, slot.count);
    }
    if (stats.callers_overflowed) {
      callers += fmt::format(" +{}more", stats.callers_overflowed);
    }
    std::string extra;
    if (kHooks[h].decode_args) {
      extra = fmt::format(
          " prim_mask={:016X} counts(<=4/<=16/<=64/<=256/<=1024/>1024)={}/{}/{}/{}/{}/{}",
          stats.prim_mask, stats.count_buckets[0], stats.count_buckets[1], stats.count_buckets[2],
          stats.count_buckets[3], stats.count_buckets[4], stats.count_buckets[5]);
    }
    REXGPU_INFO("[draw-telemetry] frame={} total={} {}{}={} callers:{}{}", frame, total,
                kHooks[h].init_time ? "INIT-TIME " : "", kHooks[h].name, stats.calls, callers,
                extra);
  }
}

bool TelemetryEnabled() {
  // Read once: the cvar is diagnostic and toggling it mid-session would make
  // the per-frame series discontinuous anyway.
  if (!g_enable_checked) {
    g_enable_checked = true;
    g_enabled_cached = REXCVAR_GET(draw_telemetry);
  }
  return g_enabled_cached;
}

void RecordDraw(size_t hook, uint32_t lr, uint32_t r4, uint32_t r5) {
  uint64_t frame = rex::perf::GuestFrameIndex();
  if (frame != g_frame) {
    if (g_frame != UINT64_MAX) {
      ReportFrame(g_frame);
    }
    for (auto& stats : g_stats) {
      stats = HookFrameStats{};
    }
    g_frame = frame;
  }

  HookFrameStats& stats = g_stats[hook];
  ++stats.calls;
  RecordCaller(stats, lr);
  if (kHooks[hook].decode_args) {
    stats.prim_mask |= uint64_t(1) << (r4 & 0x3F);
    ++stats.count_buckets[CountBucket(r5 & 0xFFFF)];
  }
}

}  // namespace

// Strong overrides preserve the original whole-function fallback when a native
// producer cannot handle the call.
#define DRAW_TELEMETRY_HOOK(index, function)                                             \
  REX_FUNC(function) {                                                                   \
    if (TelemetryEnabled()) {                                                            \
      RecordDraw(index, uint32_t(ctx.lr), ctx.r4.u32, ctx.r5.u32);                       \
    }                                                                                    \
    if (!simpsons::native_gpu_calls::TrySimpleDraw(                                      \
            simpsons::native_gpu_calls::FunctionAddress(kHooks[index].name), ctx, base)) \
      __imp__##function(ctx, base);                                                      \
  }

DRAW_TELEMETRY_HOOK(0, sub_82462810)
DRAW_TELEMETRY_HOOK(1, sub_824646E8)
DRAW_TELEMETRY_HOOK(2, sub_824626B8)
DRAW_TELEMETRY_HOOK(3, sub_82463E60)
DRAW_TELEMETRY_HOOK(4, sub_82464590)
DRAW_TELEMETRY_HOOK(5, sub_82455570)
DRAW_TELEMETRY_HOOK(6, sub_82444A48)
DRAW_TELEMETRY_HOOK(7, sub_82444A80)
DRAW_TELEMETRY_HOOK(8, sub_8244C450)
DRAW_TELEMETRY_HOOK(9, sub_8244C970)
DRAW_TELEMETRY_HOOK(10, sub_8244CF78)
DRAW_TELEMETRY_HOOK(11, sub_8244D360)
DRAW_TELEMETRY_HOOK(12, sub_8244D7B8)
DRAW_TELEMETRY_HOOK(13, sub_8244DC30)
DRAW_TELEMETRY_HOOK(14, sub_824529C0)
DRAW_TELEMETRY_HOOK(15, sub_8246AEF0)
DRAW_TELEMETRY_HOOK(16, sub_8246AF58)
DRAW_TELEMETRY_HOOK(17, sub_8246C750)
