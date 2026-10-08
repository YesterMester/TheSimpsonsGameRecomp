#pragma once

// Request native audio scheduling for the calling thread. Desktop priority
// services may grant it when the process cannot raise its priority directly.
extern "C" bool rex_audio_set_thread_priority();
