/**
 * @file        core/perf/frame_index.cpp
 * @brief       Guest-observed frame counter
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/perf/frame_index.h>

#include <atomic>

namespace rex::perf {
namespace {
std::atomic<uint64_t> g_guest_frame_index{0};
}  // namespace

uint64_t GuestFrameIndex() {
  return g_guest_frame_index.load(std::memory_order_relaxed);
}

void AdvanceGuestFrameIndex() {
  g_guest_frame_index.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace rex::perf
