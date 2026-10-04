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
// A colour other than black changes the composite's microcode, using its
// literals c248-c250, which it never reads:
// - original: instruction 110 computes the line colour as 1 - mask from c254.w,
//   the same for every channel; it reads c250.xyz = colour + 1 instead.
// - soft: instructions 73-74 compute colour * (1 - k * mask) - colour, the
//   darkening toward black; they compute (colour - ink) * -k * mask instead
//   (ink in c249, the zero in c248), so instruction 75 blends toward the ink.
// Changed microcode no longer matches the native and precompiled shaders, so
// the composite is translated at run time then; black keeps it unchanged.
//
// The literals sit in front of each shader's microcode, outside the hashed
// range, so with black ink the native and precompiled shaders still match.
// The game copies shaders and literals when it creates its effects, hence the
// patch right after the image is loaded and a restart to change it. The
// chain's master switches (0x82CD1424, 0x82CD1430) are no way to turn the ink
// off: without the composite the screen would show the packed IDs.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_STRING(ink_outlines, "original", "GPU",
                      "The black ink outlines: original (hard), soft (anti-aliased, darkness set "
                      "by ink_outline_strength) or off")
    .allowed({"original", "soft", "off"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(ink_outline_strength, 0.6, "GPU",
                      "Darkness of the soft ink outlines, 0 (none) to 1 (close to the original)")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(ink_outline_color, "000000", "GPU",
                      "Colour of the ink outlines as RRGGBB hex, 000000 = the original black")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

// simpsons_edge literal c255.w: the smallest packed-value difference that
// counts as an edge.
constexpr uint32_t kEdgeThreshold = 0x8202EB00;
constexpr float kEdgeThresholdOriginal = 1e-4f;
// simpsons_edgeAA's DepthAAThres in the post-pass settings block.
constexpr uint32_t kDepthAAThres = 0x82CD143C;
constexpr float kDepthAAThresOriginal = 0.1f;
// simpsons_edgeAA literals c248-c255 (16 bytes each) and microcode.
constexpr uint32_t kCompositeLiterals = 0x82034D48;
constexpr uint32_t kSoftDarkening = kCompositeLiterals + 5 * 16;  // c253.x
constexpr float kSoftDarkeningOriginal = -0.5f;
constexpr uint32_t kZeroLiteral = kCompositeLiterals + 0 * 16;      // c248
constexpr uint32_t kSoftInkLiteral = kCompositeLiterals + 1 * 16;   // c249
constexpr uint32_t kHardInkLiteral = kCompositeLiterals + 2 * 16;   // c250
constexpr uint32_t kCompositeMicrocode = 0x82034DC8;

// One instruction dword: where, what the game ships, what replaces it.
struct MicrocodePatch {
  uint32_t address;
  uint32_t original;
  uint32_t replacement;
};

constexpr uint32_t Instruction(uint32_t index, uint32_t dword) {
  return kCompositeMicrocode + index * 12 + dword * 4;
}

// 110: add r0.yzw, -r6.xxyz, c254.wwww -> add r0.yzw, -r6.xxyz, c250.xxyz
constexpr MicrocodePatch kHardInkPatch[] = {
    {Instruction(110, 1), 0x1CFC1B00, 0x1CFCFC00},
    {Instruction(110, 2), 0x8006FE00, 0x8006FA00},
};
// 73: add r0.yzw, r1.yyzw, c254.wwww          -> add r0.yzw, r3.xxzw, -c249.xxyz
// 74: mad r0.yzw, r0.yyzw, r3.xxzw, -r3.xxzw  -> mad r0.yzw, r1.yyzw, r0.yyzw, c248.xxxx
constexpr MicrocodePatch kSoftInkPatch[] = {
    {Instruction(73, 1), 0x18011B00, 0x1A0CFC00},
    {Instruction(73, 2), 0x8001FE00, 0x8003F900},
    {Instruction(74, 1), 0x19010C0C, 0x1801016C},
    {Instruction(74, 2), 0xEB000303, 0xCB0100F8},
};

uint32_t LoadBE32(rex::memory::Memory* memory, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, memory->TranslateVirtual(address), sizeof(value));
  return rex::byte_swap(value);
}

float LoadBEFloat(rex::memory::Memory* memory, uint32_t address) {
  uint32_t bits = LoadBE32(memory, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// Writes to the loaded image, lifting the read-only protection of .rdata
// pages for the write.
void StoreBE32(rex::memory::Memory* memory, uint32_t address, uint32_t value) {
  value = rex::byte_swap(value);
  rex::memory::BaseHeap* heap = memory->LookupHeap(address);
  const uint32_t page = address & ~(heap->page_size() - 1);
  uint32_t old_protect = 0;
  heap->Protect(page, heap->page_size(),
                rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, &old_protect);
  std::memcpy(memory->TranslateVirtual(address), &value, sizeof(value));
  heap->Protect(page, heap->page_size(), old_protect);
}

void StoreBEFloat(rex::memory::Memory* memory, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  StoreBE32(memory, address, bits);
}

void StoreLiteral(rex::memory::Memory* memory, uint32_t address, float x, float y, float z,
                  float w) {
  StoreBEFloat(memory, address + 0, x);
  StoreBEFloat(memory, address + 4, y);
  StoreBEFloat(memory, address + 8, z);
  StoreBEFloat(memory, address + 12, w);
}

bool LiteralIsZero(rex::memory::Memory* memory, uint32_t address) {
  for (uint32_t i = 0; i < 16; i += 4) {
    if (LoadBE32(memory, address + i) != 0) {
      return false;
    }
  }
  return true;
}

template <size_t N>
bool PatchMatches(rex::memory::Memory* memory, const MicrocodePatch (&patch)[N]) {
  for (const MicrocodePatch& p : patch) {
    if (LoadBE32(memory, p.address) != p.original) {
      return false;
    }
  }
  return true;
}

template <size_t N>
void ApplyPatch(rex::memory::Memory* memory, const MicrocodePatch (&patch)[N]) {
  for (const MicrocodePatch& p : patch) {
    StoreBE32(memory, p.address, p.replacement);
  }
}

// "RRGGBB" or "#RRGGBB" to 0-1 components; false if malformed.
bool ParseColor(std::string text, float rgb[3]) {
  if (!text.empty() && text[0] == '#') {
    text.erase(0, 1);
  }
  if (text.size() != 6) {
    return false;
  }
  char* end = nullptr;
  const unsigned long value = std::strtoul(text.c_str(), &end, 16);
  if (end != text.c_str() + 6) {
    return false;
  }
  rgb[0] = float((value >> 16) & 0xFF) / 255.0f;
  rgb[1] = float((value >> 8) & 0xFF) / 255.0f;
  rgb[2] = float(value & 0xFF) / 255.0f;
  return true;
}

}  // namespace

void ApplyInkOutlineOptions(rex::memory::Memory* memory) {
  const std::string mode = REXCVAR_GET(ink_outlines);
  float ink[3] = {0.0f, 0.0f, 0.0f};
  if (!ParseColor(REXCVAR_GET(ink_outline_color), ink)) {
    REXLOG_WARN("ink_outline_color: \"{}\" is not RRGGBB, using black",
                REXCVAR_GET(ink_outline_color));
  }
  const bool colored = ink[0] != 0.0f || ink[1] != 0.0f || ink[2] != 0.0f;
  if (mode == "original" && !colored) {
    return;
  }
  // Another release of the game has its data elsewhere: change nothing.
  if (LoadBEFloat(memory, kEdgeThreshold) != kEdgeThresholdOriginal ||
      LoadBEFloat(memory, kDepthAAThres) != kDepthAAThresOriginal ||
      LoadBEFloat(memory, kSoftDarkening) != kSoftDarkeningOriginal ||
      !LiteralIsZero(memory, kZeroLiteral) || !LiteralIsZero(memory, kSoftInkLiteral) ||
      !LiteralIsZero(memory, kHardInkLiteral) || !PatchMatches(memory, kHardInkPatch) ||
      !PatchMatches(memory, kSoftInkPatch)) {
    REXLOG_WARN("ink_outlines: this game image is not the one the option was made for; "
                "outlines left as they are");
    return;
  }
  if (mode == "off") {
    StoreBEFloat(memory, kEdgeThreshold, 2.0f);
    REXLOG_INFO("ink_outlines: off");
    return;
  }
  if (mode == "soft") {
    const double strength = REXCVAR_GET(ink_outline_strength);
    StoreBEFloat(memory, kDepthAAThres, 2.0f);
    // 5/3 makes the default EdgeColorScale (1.5) darken by 1.25 * strength *
    // depth fade * blurred mask, where the blurred mask is about 0.6 to 0.8
    // on a line and 0.2 beside it.
    StoreBEFloat(memory, kSoftDarkening, float(kSoftDarkeningOriginal * (5.0 / 3.0) * strength));
    if (colored) {
      StoreLiteral(memory, kSoftInkLiteral, ink[0], ink[1], ink[2], 0.0f);
      ApplyPatch(memory, kSoftInkPatch);
    }
    REXLOG_INFO("ink_outlines: soft, strength {:.2f}, colour {}", strength,
                REXCVAR_GET(ink_outline_color));
    return;
  }
  // original (hard) with a colour
  StoreLiteral(memory, kHardInkLiteral, 1.0f + ink[0], 1.0f + ink[1], 1.0f + ink[2], 1.0f);
  ApplyPatch(memory, kHardInkPatch);
  REXLOG_INFO("ink_outlines: original, colour {}", REXCVAR_GET(ink_outline_color));
}
