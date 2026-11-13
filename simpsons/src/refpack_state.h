#pragma once

namespace simpsons::native_refpack {
// Guest-memory restore rewinds decoder cursors. Discard host progress before
// resuming it; untracked streams continue through the original routine.
void InvalidateStreams();
}  // namespace simpsons::native_refpack
