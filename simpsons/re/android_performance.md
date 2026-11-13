# Android performance

## Frame waiting and scheduling

Android's limiter uses `AdaptiveFrameWait` to learn the scheduler's sleep
overshoot. The guard starts at 100 microseconds, grows immediately after a
delayed wake-up and shrinks gradually toward a 50-microsecond minimum. Its
maximum is 2 ms. After every sleep or yield, the limiter checks the same guest
deadline it used previously. Simulation timing, the vblank grid and configured
frame rates remain the sources of those deadlines.

This avoids spending the final 2 ms of every light frame repeatedly yielding
a CPU core. It applies to both whole-vblank rates and fractional frame rates
on Android. Desktop wait behavior remains as before.

The runtime's [Android performance-hint session](https://developer.android.com/ndk/reference/group/a-performance-hint)
retains its device-managed scheduling. Activity transitions discard the sample
that can span a pause and recreate the session. Main-thread registration and
frame-work timestamps are thread-local. Registered workers remove themselves
when they exit, so replacement workers do not leave dead thread IDs in a
session. Android 14's thread-update operation can fall back to session
recreation; Android 13 recreates directly.

Manager, creation, target-update and report failures can be retried after a
one-second backoff. A failed target update does not publish a successful target
or report against it. Repeated creation failures log once until a session
succeeds. Missing APIs and the disabled setting remain supported.

These paths use the system's capabilities rather than a fixed CPU-core map or
a model-name preset. Tensor and Snapdragon still need their own hardware
measurements, including sustained sessions and return from the background.

## Vulkan uploads

When an upload buffer expands into allocation padding, memory selection now
uses the expanded buffer's `memoryTypeBits`. The original buffer's compatible
types need not describe its replacement. Unsupported expansion still keeps
the original buffer. This fixes a driver-compatibility error; it has no measured
gameplay FPS result on an affected phone yet.

## November 4, 2026 checks

`tools/bench/android_frame_wait_check.cpp` exercises the production wait policy
at 24, 25, 30, 45, 60, 90, 120, 144, 240 and 1000 FPS. It covers ordinary and
delayed wake-ups, recovery, expired deadlines and extreme observations:
2,348,711 checks pass. Build it with a C++20 compiler:

```sh
c++ -std=c++20 -O2 -Wall -Wextra -Werror \
    tools/bench/android_frame_wait_check.cpp -o /tmp/android-frame-wait-check
/tmp/android-frame-wait-check --measure
```

The optional timing run waits for 120 deadlines at 120 Hz with each policy.
On the Steam Deck, the original fixed guard used 228.6 ms of CPU time during
its one-second run; the adaptive guard used 8.4 ms. The respective 99th-percentile
lateness was 1.17 and 1.74 microseconds. This measures limiter overhead on the
host, and is separate from phone FPS or power measurements.

`tools/bench/android_performance_hint_check.cpp` runs the production Android
session code on Linux against a controlled service. Its 176 checks cover the
Android 13 and 14 API paths, absent and disabled APIs, duplicate registration,
worker retirement, pause/resume, rejected thread updates, missing managers and
failed creation, target update and reporting. It links a matching runtime and
uses the linker's `--wrap=dlopen,--wrap=dlsym` to substitute only the service.
This checks session behavior; it does not reproduce a phone's scheduler.

The game and runtime also build as Android ARM64. Packaging checks the actual
native dependencies, ELF segments, APK alignment and signing certificate.

## Deck FPS baseline

The latest private native-GPU build was measured at the Springfield bus stop
with a 1280 by 720 presentation window, a 60 FPS gameplay cap and a 30 FPS menu
cap. Each gameplay sample lasts 30 seconds after loading and settling. Audio
output is muted, native route diagnostics are disabled and performance counters
are enabled. All three runs exit cleanly.

| Render scale | Mean FPS | 99th-percentile frame time |
| --- | ---: | ---: |
| 1x | 59.77 | 17.60 ms |
| 2x | 59.76 | 17.60 ms |
| 3x | 32.24 | 32.91 ms |

The 3x sample keeps the GPU fully busy while command-processor CPU work averages
about 8.1 ms per frame. That scene needs less GPU work to reach 60 FPS at 3x.
The title holds its 30 FPS cap at all three scales. These samples cover one
idle scene on the Deck's RADV driver. They do not establish campaign-wide
frame-time limits or Tensor/Snapdragon FPS.

For phone comparisons, keep the same save, camera, render scale and frame cap.
Measure both a warm-cache session and several minutes of movement, effects and
co-op. Keep startup shader preparation separate. Collect the Android support
bundle after audio or performance trouble, and record the model, GPU driver,
temperature behavior and time spent playing alongside FPS.
