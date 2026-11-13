/**
 ******************************************************************************
 * ReXGlue - Xbox 360 Static Recompilation Runtime                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <rex/graphics/util/buffer_hash.h>
#include <rex/graphics/util/bytes_equal.h>
#include <rex/hash.h>

namespace rex::graphics::draw_util {

// Small dynamic streams are cheaper to compare directly. Large immutable
// streams keep a full-content hash, avoiding a second large memory read. A
// changed prefix rejects animated data before hashing the entire stream.
inline uint64_t HashBufferContent(const void* bytes, size_t size) {
  return size <= 256 ? 0 : HashNativeBuffer(bytes, size);
}

inline bool BufferContentMatches(const void* source, const void* snapshot, size_t size,
                                 uint64_t hash) {
  if (size <= 256) {
    return BytesEqual(source, snapshot, size);
  }
  return BytesEqual(source, snapshot, 64) && HashNativeBuffer(source, size) == hash;
}

}  // namespace rex::graphics::draw_util
