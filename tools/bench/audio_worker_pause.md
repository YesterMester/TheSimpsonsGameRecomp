# Audio worker pause regression

`audio_shutdown_check.cpp` exercises the real audio worker, function dispatcher,
XMA worker and shutdown path without game data. Its driver is a test boundary;
callbacks execute as native mapped functions and keep their normal worker lifetime.

The `busy` mode continuously returns a client semaphore permit, modeling a device
that keeps consuming frames while pause is requested. Client handles precede the
shutdown/pause event in `AudioSystem::wait_handles_`. The old loop acknowledged
pause only when `WaitAny` selected that last event, so a continuously ready client
could prevent pause from completing.

The corrected worker checks the atomic pause request before selecting another
client. A callback already selected is allowed to finish and use its permit.
The event still wakes an idle worker, and shutdown still releases a paused worker.
Callback pacing, queue size, mixing and decoder behavior retain their existing code.

Build with the runtime's headers and C++ ABI:

The helper compiles the fixture only. Rebuild the runtime separately when testing
its current source.

```sh
sh tools/bench/build_audio_shutdown_check.sh /path/to/audio_shutdown_check
/path/to/audio_shutdown_check idle /path/to/idle.json
/path/to/audio_shutdown_check active /path/to/active.json
/path/to/audio_shutdown_check paused /path/to/paused.json
/path/to/audio_shutdown_check busy /path/to/busy.json
```

Run each process under a short external timeout. With the old runtime, `idle`
and `active` pass, while `busy` reaches its callback and then exceeds six seconds
without acknowledging pause. With the corrected production methods, all four
modes pass. `busy` performs 20 pause/resume cycles, exceeding the fixed eight-slot
queue capacity, and requires playback to progress after each resume. This also
catches permit loss. All started callbacks must finish before the driver is
destroyed, and repeated shutdown must not destroy it twice.

Private Linux qualification used the pinned game runtime with an audio-only
shared-library slice of the corrected Setup, worker, Pause and Resume methods.
Only entry diagnostics differ from those method bodies; the renderer and decoder
stay pinned. This avoids rebuilding or replacing another renderer test's runtime.
The busy regression completes 20 cycles with a maximum 80 ms pause, matching its
80 ms callback, and cooperative shutdown completes in 79 ms.

A private Linux game run with that slice also boots a checkpoint, renders an
owned skinned NPC, resumes simulation, removes it and exits 0. The audio pause
step finishes before title and graphics teardown. Native Windows/Pixel behavior
and other shutdown stalls remain separate qualification; this fixes the specific
ready-client starvation case rather than relying on the exit watchdog.
