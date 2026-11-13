#pragma once
/**
 * @file        ring_progress.h
 * @brief       Waiting for the command processor to advance through the ring buffer.
 *
 * The game's Direct3D code waits for the GPU to consume its command ring
 * before reusing command memory, polling the read pointer the command
 * processor writes back to guest memory. On the Xbox 360 that poll paused the
 * hardware thread between checks; recompiled, it spins a host core flat out,
 * which on a power-limited APU takes power from the GPU. A hook can instead
 * block until the command processor next makes progress visible to the guest:
 * a read pointer, scratch register or shader-event completion writeback (the
 * fence values the Direct3D code waits on).
 */

#include <cstdint>

namespace rex::graphics {

// How many times the command processor has written ring progress back to
// guest memory (read pointer, scratch register or shader-event writebacks).
uint32_t GetRingProgressCount();

// Blocks until the progress count differs from seen_count, or timeout_us
// passes.
void WaitForRingProgress(uint32_t seen_count, uint32_t timeout_us);

// Guest physical address the read pointer is written back to (0 if not set).
uint32_t GetReadPointerWritebackAddress();

}  // namespace rex::graphics
