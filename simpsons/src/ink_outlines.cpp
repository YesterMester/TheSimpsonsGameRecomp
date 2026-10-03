// Ink outline options.
//
// The game's black outlines come from a chain of three full-screen passes run
// every frame (sub_823C7500). simpsons_edge (pixel shader 6116B6CC7219148B)
// marks the pixels where the object ID or the palette index of the packed
// world buffer changes. simpsons_aa (380049563CA2AB0A) blurs that mask.
// simpsons_edgeAA (346B23B40037EB24), which also resolves the frame's colours
// from the palette, draws the ink: on a line pixel it blends to pure black,
// whatever EdgeColorScale is, and only below its DepthAAThres does it darken
// softly, by 0.5 * EdgeColorScale * depth fade * blurred mask.
//
// "off" raises the edge pass's difference threshold (shader literal c255.w,
// 1e-4) above any difference two packed values can have, so the mask stays
// empty and the composite keeps the palette, shadow and rim colours alone.
// "soft" sends every line pixel down the soft branch (DepthAAThres, read every
// frame from .data and written by nothing else, 0.1 -> 2.0) and scales its
// darkening (the composite's literal c253.x, -0.5) by the strength: at 1 the
// line core stays almost black with a lighter rim, at 0.5 it darkens the
// colour underneath by about half.
//
// The literals sit in front of each shader's microcode, outside the hashed
// range, so the native and precompiled shaders still match. The game copies
// them when it creates its effects, hence the patch right after the image is
// loaded and a restart to change it. The chain's master switches (0x82CD1424,
// 0x82CD1430) are no way to turn the ink off: without the composite the screen
// would show the packed IDs.

#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_STRING(ink_outlines, "original", "GPU",
                      "The black ink outlines: original, soft (anti-aliased, darkness set by "
                      "ink_outline_strength) or off")
    .allowed({"original", "soft", "off"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(ink_outline_strength, 0.6, "GPU",
                      "Darkness of the soft ink outlines, 0 (none) to 1 (close to the original)")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

// simpsons_edge literal c255.w: the smallest packed-value difference that
// counts as an edge.
constexpr uint32_t kEdgeThreshold = 0x8202EB00;
constexpr float kEdgeThresholdOriginal = 1e-4f;
// simpsons_edgeAA's DepthAAThres in the post-pass settings block.
constexpr uint32_t kDepthAAThres = 0x82CD143C;
constexpr float kDepthAAThresOriginal = 0.1f;
// simpsons_edgeAA literal c253.x: the soft branch's darkening factor.
constexpr uint32_t kSoftDarkening = 0x82034D98;
constexpr float kSoftDarkeningOriginal = -0.5f;

float LoadBEFloat(rex::memory::Memory* memory, uint32_t address) {
  uint32_t bits;
  std::memcpy(&bits, memory->TranslateVirtual(address), sizeof(bits));
  bits = rex::byte_swap(bits);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// Writes a float to the loaded image, lifting the read-only protection of
// .rdata pages for the write.
void StoreBEFloat(rex::memory::Memory* memory, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  bits = rex::byte_swap(bits);
  rex::memory::BaseHeap* heap = memory->LookupHeap(address);
  const uint32_t page = address & ~(heap->page_size() - 1);
  uint32_t old_protect = 0;
  heap->Protect(page, heap->page_size(),
                rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, &old_protect);
  std::memcpy(memory->TranslateVirtual(address), &bits, sizeof(bits));
  heap->Protect(page, heap->page_size(), old_protect);
}

}  // namespace

void ApplyInkOutlineOptions(rex::memory::Memory* memory) {
  const std::string mode = REXCVAR_GET(ink_outlines);
  if (mode != "soft" && mode != "off") {
    return;
  }
  // Another release of the game has its data elsewhere: change nothing.
  if (LoadBEFloat(memory, kEdgeThreshold) != kEdgeThresholdOriginal ||
      LoadBEFloat(memory, kDepthAAThres) != kDepthAAThresOriginal ||
      LoadBEFloat(memory, kSoftDarkening) != kSoftDarkeningOriginal) {
    REXLOG_WARN("ink_outlines: this game image is not the one the option was made for; "
                "outlines left as they are");
    return;
  }
  if (mode == "off") {
    StoreBEFloat(memory, kEdgeThreshold, 2.0f);
    REXLOG_INFO("ink_outlines: off");
    return;
  }
  const double strength = REXCVAR_GET(ink_outline_strength);
  StoreBEFloat(memory, kDepthAAThres, 2.0f);
  // 5/3 makes the default EdgeColorScale (1.5) darken by 1.25 * strength *
  // depth fade * blurred mask, where the blurred mask is about 0.6 to 0.8 on
  // a line and 0.2 beside it.
  StoreBEFloat(memory, kSoftDarkening, float(kSoftDarkeningOriginal * (5.0 / 3.0) * strength));
  REXLOG_INFO("ink_outlines: soft, strength {:.2f}", strength);
}
