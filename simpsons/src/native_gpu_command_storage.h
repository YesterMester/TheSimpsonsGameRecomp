#pragma once

#include <cstddef>

namespace simpsons::native_gpu_memory {

// Guest-writable pages are recovered by the runtime's access-violation
// handler. Forcing host permissions here would remove GPU write tracking.
// Share the original no-op policy with the native chunk-growth path.
inline bool EnsureCommandStorageWritable(void* host_address, size_t bytes) {
  (void)host_address;
  (void)bytes;
  return true;
}

}  // namespace simpsons::native_gpu_memory
