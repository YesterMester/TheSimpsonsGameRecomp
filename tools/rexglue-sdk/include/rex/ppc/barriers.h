#pragma once

#include <atomic>

// PowerPC memory barriers in recompiled code. The Xenon, like ARM, may reorder
// memory accesses between cores, and the game's threads (its loaders and its
// video decoder among them) publish data behind sync / lwsync / eieio. x86
// already keeps the orders those barriers ask for, so there they only stop the
// compiler moving accesses across them; ARM needs the fences themselves.
#if defined(__aarch64__) || defined(_M_ARM64)
#define REX_PPC_SYNC() std::atomic_thread_fence(std::memory_order_seq_cst)
#define REX_PPC_LWSYNC() std::atomic_thread_fence(std::memory_order_acq_rel)
#define REX_PPC_EIEIO() std::atomic_thread_fence(std::memory_order_seq_cst)
#else
#define REX_PPC_SYNC() std::atomic_signal_fence(std::memory_order_seq_cst)
#define REX_PPC_LWSYNC() std::atomic_signal_fence(std::memory_order_seq_cst)
#define REX_PPC_EIEIO() std::atomic_signal_fence(std::memory_order_seq_cst)
#endif
