/**
 * @file        perf/event_trace.h
 * @brief       Debugging timeline of frame events
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>

namespace rex::perf {

// REX_EVENT_TRACE=<path> records events with CLOCK_MONOTONIC microseconds and
// the thread id, and appends them to <path> as "time_us tid name arg" lines
// (written by FlushEventTrace). Without it, TraceEvent does nothing.
bool EventTraceEnabled();
void TraceEvent(const char* name, uint64_t arg = 0);
// Marks the calling thread as the game's swapping thread (the one calling
// VdSwap), for TraceSwapThreadEvent.
void MarkSwapThread();
// TraceEvent, only on the game's swapping thread.
void TraceSwapThreadEvent(const char* name, uint64_t arg = 0);
// Writes the recorded events. Called regularly by one thread.
void FlushEventTrace();

}  // namespace rex::perf
