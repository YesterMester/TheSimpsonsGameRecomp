#pragma once

// Request native audio scheduling for the calling thread. Desktop priority
// services may grant it when the process cannot raise its priority directly.
extern "C" bool rex_audio_set_thread_priority();

// Briefly block at a safe audio work boundary before a continuous real-time
// CPU burst exhausts the host's budget. Returns true when it slept. This is
// inactive for threads without real-time audio scheduling and outside Linux.
extern "C" bool rex_audio_yield_realtime_budget();
