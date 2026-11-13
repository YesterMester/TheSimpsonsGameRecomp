/**
 ******************************************************************************
 * ReXGlue - Xbox 360 Static Recompilation Runtime                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstdint>

#include <rex/platform.h>

namespace rex::platform {

// Android's performance hint session (Android 13+): the system learns how long
// each frame's work takes against its deadline, and gives the session's
// threads the CPU cores and clocks to finish in time - no more, so a light
// frame saves power and a heavy one is boosted before it misses the deadline.
// Elsewhere, and on older Android versions, these do nothing.
#if REX_PLATFORM_ANDROID
// Adds the calling thread to the threads the frame deadline depends on.
void AddPerformanceHintThread();
// Do not report work spanning an activity pause as one long, overloaded frame.
void SetPerformanceHintActive(bool active);
// Reports one frame: how long its work took, and the frame time it had.
// Called from one thread, once per frame.
void ReportFrameWork(int64_t work_ns, int64_t target_ns);
#else
inline void AddPerformanceHintThread() {}
inline void SetPerformanceHintActive(bool) {}
inline void ReportFrameWork(int64_t, int64_t) {}
#endif

}  // namespace rex::platform
