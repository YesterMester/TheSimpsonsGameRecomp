// Drawing with the game's own 2D renderer (game_canvas.h).
//
// The font manager (0x82D08EA8) batches screen-space quads: 28-byte vertices
// (x, y, z, reciprocal depth, ARGB colour, u, v) at +68, their count at +72
// and room at +74, drawn by sub_826BE8C0 with the current texture. Its camera
// at +44 has the target raster at +96 (width +12, height +16). A font has its
// line height at +24, its glyph table at +36 (one pointer per character: size
// at +2/+4, advance at +6, offset at +8/+10, atlas rectangle at +12..+18 in
// sixteenths of a texel) and its atlas texture at +1060. Text draws turn on
// vertex alpha and alpha blending and turn fog off through RenderWare's render
// state calls (the device at 0x82D0CA68: set at +32, get at +36); this does the
// same, keeps the depth test off, and puts everything back afterwards. Solid
// shapes sample the middle of the underscore, which is opaque white.
//
// Pictures are textures of our own: a RenderWare raster (52 bytes: size at
// +12/+16, depth +20, type and format bytes at +32/+35), the Xbox 360
// driver's raster extension (+52 the D3D texture, +60 has-alpha and
// compressed flags, +76 the D3DFORMAT) and the D3D texture header inline at
// +84 (fetch constant at +112), all copied from the font's raster and changed
// to a linear A8R8G8B8 texture in physical memory, which the GPU reads like
// any other. Writing new pixels trips the renderer's write watch on those
// pages, so it reloads them. Pictures go into the slot drawn longest ago, so a
// texture the GPU may still be reading for the last frame isn't overwritten.

#include "game_canvas.h"

#include "game_thread.h"
#include "guest_memory.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include <rex/logging.h>
#include <rex/ppc.h>

namespace canvas {
namespace {

using simpsons::GuestToHost;
using simpsons::LoadGuestFloat;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestFloat;
using simpsons::StoreGuestU32;

constexpr uint32_t kFontManager = 0x82D08EA8;
constexpr uint32_t kRenderDevice = 0x82D0CA68;
constexpr uint32_t kFlush = 0x826BE8C0;
// RenderWare render states.
constexpr uint32_t kTextureRaster = 1, kZTest = 6, kZWrite = 8, kSrcBlend = 10, kDestBlend = 11,
                   kVertexAlpha = 12, kFog = 14;
constexpr uint32_t kSavedStates[] = {kZTest, kZWrite, kSrcBlend, kDestBlend, kVertexAlpha, kFog};

uint16_t U16(const uint8_t* base, uint32_t address) {
  const auto* p = GuestToHost(base, address);
  return uint16_t(p[0] << 8 | p[1]);
}
void Put16(uint8_t* base, uint32_t address, uint16_t value) {
  auto* p = GuestToHost(base, address);
  p[0] = uint8_t(value >> 8);
  p[1] = uint8_t(value);
}
uint32_t Glyph(const uint8_t* base, uint32_t font, uint8_t character) {
  const uint32_t table = font ? LoadGuestU32(base, font + 36) : 0;
  return table ? LoadGuestU32(base, table + character * 4u) : 0;
}
bool Usable(const uint8_t* base, uint32_t font) {
  if (!font || !LoadGuestU32(base, font + 1060))
    return false;
  const uint32_t letter = Glyph(base, font, 'A'), white = Glyph(base, font, '_');
  return letter && white && U16(base, letter + 2) > 1 && U16(base, letter + 4) > 1 &&
         U16(base, white + 2) > 1 && U16(base, white + 4) > 1 && LoadGuestU32(base, font + 24) > 4;
}

// The game's fonts have a glyph per byte, Latin-1's: UTF-8 text is turned
// into that, with "?" for what Latin-1 lacks.
std::string Latin1(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const uint8_t b = uint8_t(text[i]);
    if (b < 0x80) {
      out += char(b);
      continue;
    }
    const size_t extra = (b & 0xE0) == 0xC0   ? 1
                         : (b & 0xF0) == 0xE0 ? 2
                         : (b & 0xF8) == 0xF0 ? 3
                                              : 0;
    if (!extra)
      continue;  // not the start of a character
    uint32_t code = b & (0x3F >> extra);
    size_t j = 1;
    for (; j <= extra && i + j < text.size() && (uint8_t(text[i + j]) & 0xC0) == 0x80; ++j)
      code = code << 6 | (uint8_t(text[i + j]) & 0x3F);
    i += j - 1;
    if (j > extra)  // whole, not cut off
      out += code < 0x100 ? char(code) : '?';
  }
  return out;
}

struct WidgetState {
  std::array<float, 6> transform{1, 0, 0, 1, 0, 0};
  std::array<float, 4> clip{};
  float alpha = 1;
  bool enabled = false;
};
thread_local WidgetState g_widget;

uint32_t g_scratch = 0;  // guest memory for render state reads
uint32_t g_vertices = 0, g_vertex_capacity = 0;

// Pictures: rasters in the first page, then each slot's pixels.
constexpr uint32_t kSlotWidth = 512, kSlotHeight = 288, kSlots = 3;
constexpr uint32_t kSlotBytes = kSlotWidth * kSlotHeight * 4;
constexpr uint32_t kRasterBytes = 136, kTextureHeader = 84;
static_assert(kPictureMemory == 4096 + kSlots * kSlotBytes);
struct Slot {
  uint64_t id = 0;
  uint64_t drawn = 0;  // when it was last drawn (canvases made)
  uint32_t width = 0, height = 0;
};
uint32_t g_pictures = 0;
bool g_pictures_built = false, g_pictures_failed = false;
Slot g_slots[kSlots];
uint64_t g_canvases = 0;

uint32_t SlotRaster(uint32_t slot) {
  return g_pictures + slot * 256;
}
uint32_t SlotPixels(uint32_t slot) {
  return g_pictures + 4096 + slot * kSlotBytes;
}

// The rasters, from the font's: false if it isn't laid out as expected.
bool BuildPictures(uint8_t* base, uint32_t font_raster) {
  if (LoadGuestU32(base, font_raster + 52) != font_raster + kTextureHeader ||
      (LoadGuestU32(base, font_raster + kTextureHeader + 28) & 3) != 2) {
    REXLOG_WARN("[canvas] the font's raster isn't laid out as expected; no pictures");
    return false;
  }
  for (uint32_t slot = 0; slot < kSlots; ++slot) {
    const uint32_t raster = SlotRaster(slot), texture = raster + kTextureHeader;
    std::memcpy(GuestToHost(base, raster), GuestToHost(base, font_raster), kRasterBytes);
    StoreGuestU32(base, raster + 0, raster);  // parent: itself
    StoreGuestU32(base, raster + 4, 0);       // pixels while locked
    StoreGuestU32(base, raster + 8, 0);       // palette
    StoreGuestU32(base, raster + 12, kSlotWidth);
    StoreGuestU32(base, raster + 16, kSlotHeight);
    StoreGuestU32(base, raster + 20, 32);          // depth
    StoreGuestU32(base, raster + 24, 0);           // stride
    StoreGuestU32(base, raster + 28, 0);           // sub-raster offset
    StoreGuestU32(base, raster + 32, 0x04000005);  // a texture, 8888
    StoreGuestU32(base, raster + 36, 0);
    StoreGuestU32(base, raster + 40, kSlotWidth);
    StoreGuestU32(base, raster + 44, kSlotHeight);
    StoreGuestU32(base, raster + 48, 0);
    StoreGuestU32(base, raster + 52, texture);
    StoreGuestU32(base, raster + 56, 0);           // palette
    StoreGuestU32(base, raster + 60, 0x01000000);  // has alpha, not compressed
    StoreGuestU32(base, raster + 64, 0);
    StoreGuestU32(base, raster + 68, 0);
    StoreGuestU32(base, raster + 72, 0);
    StoreGuestU32(base, raster + 76, 0x18280086);  // D3DFMT_LIN_A8R8G8B8
    StoreGuestU32(base, raster + 80, 0);
    // The D3D texture: common and reference count as the font's, no fences.
    StoreGuestU32(base, texture + 4, 1);
    StoreGuestU32(base, texture + 8, 0);
    StoreGuestU32(base, texture + 12, 0);
    StoreGuestU32(base, texture + 16, 0);
    StoreGuestU32(base, texture + 20, 0xFFFF0000);
    StoreGuestU32(base, texture + 24, 0xFFFF0000);
    // The fetch constant: a 2D texture, clamped to its edges, rows of 512
    // texels, linear; k_8_8_8_8 swapped in 32-bit words, at the slot's
    // physical address (the GPU sees 0xE0000000 pages one page up);
    // 512x288; ARGB's swizzle and bilinear filtering.
    const uint32_t pixels = SlotPixels(slot);
    const uint32_t gpu_address = pixels >= 0xE0000000u ? pixels + 0x1000 : pixels;
    StoreGuestU32(base, texture + 28, 2 | 2u << 10 | 2u << 13 | (kSlotWidth / 32) << 22);
    StoreGuestU32(base, texture + 32, 6 | 2u << 6 | (gpu_address & 0xFFFFF000u));
    StoreGuestU32(base, texture + 36, (kSlotWidth - 1) | (kSlotHeight - 1) << 13);
    StoreGuestU32(base, texture + 40, (2 | 1u << 3 | 0u << 6 | 3u << 9) << 1 | 1u << 19 | 1u << 21);
    StoreGuestU32(base, texture + 44, 0);
    StoreGuestU32(base, texture + 48, 1u << 9);
  }
  REXLOG_INFO("[canvas] pictures at {:08X}", g_pictures);
  return true;
}

// Puts a picture's pixels in a slot, as big-endian ARGB words.
void Upload(uint8_t* base, uint32_t slot, const Picture& picture) {
  const uint32_t width = std::min(picture.width, kSlotWidth);
  const uint32_t height = std::min(picture.height, kSlotHeight);
  // Cut down by skipping pixels when it's larger than the slot.
  const float step_x = picture.width > kSlotWidth ? float(picture.width) / kSlotWidth : 1.0f;
  const float step_y = picture.height > kSlotHeight ? float(picture.height) / kSlotHeight : 1.0f;
  std::vector<uint8_t> row(kSlotWidth * 4);
  uint8_t* pixels = GuestToHost(base, SlotPixels(slot));
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* source = picture.rgba.data() + size_t(uint32_t(y * step_y)) * picture.width * 4;
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* p = source + size_t(uint32_t(x * step_x)) * 4;
      row[x * 4 + 0] = p[3];
      row[x * 4 + 1] = p[0];
      row[x * 4 + 2] = p[1];
      row[x * 4 + 3] = p[2];
    }
    // The edge repeats into the padding, for filtering at the border.
    for (uint32_t x = width; x < kSlotWidth && width; ++x)
      std::memcpy(&row[x * 4], &row[(width - 1) * 4], 4);
    std::memcpy(pixels + size_t(y) * kSlotWidth * 4, row.data(), row.size());
  }
  if (height && height < kSlotHeight)
    std::memcpy(pixels + size_t(height) * kSlotWidth * 4,
                pixels + size_t(height - 1) * kSlotWidth * 4, kSlotWidth * 4);
  g_slots[slot] = {picture.id, g_canvases, width, height};
}

}  // namespace

void Init(uint32_t scratch, uint32_t vertices, uint32_t vertex_capacity, uint32_t pictures) {
  g_scratch = scratch;
  g_vertices = vertices;
  g_vertex_capacity = vertex_capacity;
  g_pictures = pictures;
}

Canvas::Canvas(PPCContext& ctx, uint8_t* base) : ctx_(ctx), base_(base) {
  ++g_canvases;
  manager_ = LoadGuestU32(base, kFontManager);
  camera_ = manager_ ? LoadGuestU32(base, manager_ + 44) : 0;
  const uint32_t raster = camera_ ? LoadGuestU32(base, camera_ + 96) : 0;
  if (!g_scratch || !g_vertices || !raster || !LoadGuestU32(base, kRenderDevice))
    return;
  width_ = float(LoadGuestU32(base, raster + 12));
  height_ = float(LoadGuestU32(base, raster + 16));
  if (!(width_ >= 64 && height_ >= 64 && width_ <= 16384 && height_ <= 16384))
    return;
  z_ = LoadGuestFloat(base, manager_ + 52);
  const float near_z = LoadGuestFloat(base, camera_ + 144),
              range = LoadGuestFloat(base, camera_ + 140);
  depth_ = range != 0 ? (z_ - near_z) / range : 1.0f;
  if (!std::isfinite(depth_))
    depth_ = 1.0f;
  Prepare(LoadGuestU32(base, manager_ + 28));
  if (!font_) {
    for (const uint32_t font : Fonts()) {
      Prepare(font);
      if (font_)
        break;
    }
  }
  if (!font_)
    return;
  // Whatever the game queued goes out with its own settings first; then the
  // quads go into a buffer of our own.
  Flush();
  saved_buffer_ = LoadGuestU32(base, manager_ + 68);
  saved_count_ = U16(base, manager_ + 72);
  saved_capacity_ = U16(base, manager_ + 74);
  StoreGuestU32(base, manager_ + 68, g_vertices);
  Put16(base, manager_ + 72, 0);
  Put16(base, manager_ + 74, uint16_t(std::min(g_vertex_capacity, 65535u)));
  saved_raster_ = State(kTextureRaster);
  for (size_t i = 0; i < std::size(kSavedStates); ++i)
    saved_states_[i] = State(kSavedStates[i]);
  SetState(kZTest, 0);
  SetState(kZWrite, 0);
  SetState(kVertexAlpha, 1);
  SetState(kFog, 0);
  SetState(kSrcBlend, 5);   // source alpha
  SetState(kDestBlend, 6);  // inverse source alpha
  Bind(raster_);
  valid_ = true;
}

Canvas::~Canvas() {
  if (!valid_)
    return;
  Flush();
  StoreGuestU32(base_, manager_ + 68, saved_buffer_);
  Put16(base_, manager_ + 72, uint16_t(saved_count_));
  Put16(base_, manager_ + 74, uint16_t(saved_capacity_));
  for (size_t i = 0; i < std::size(kSavedStates); ++i)
    SetState(kSavedStates[i], saved_states_[i]);
  Bind(saved_raster_);
}

void Canvas::Prepare(uint32_t font) {
  if (!Usable(base_, font))
    return;
  const uint32_t texture = LoadGuestU32(base_, font + 1060);
  const uint32_t raster = texture ? LoadGuestU32(base_, texture) : 0;
  if (!raster)
    return;
  const float atlas_width = float(LoadGuestU32(base_, raster + 12));
  const float atlas_height = float(LoadGuestU32(base_, raster + 16));
  if (!(atlas_width > 0 && atlas_height > 0))
    return;
  const uint32_t white = Glyph(base_, font, '_');
  font_ = font;
  raster_ = raster;
  atlas_width_ = atlas_width;
  atlas_height_ = atlas_height;
  white_u_ = (U16(base_, white + 12) + U16(base_, white + 16)) * 0.5f / (16.0f * atlas_width);
  white_v_ = (U16(base_, white + 14) + U16(base_, white + 18)) * 0.5f / (16.0f * atlas_height);
}

std::vector<uint32_t> Canvas::Fonts() const {
  std::vector<uint32_t> fonts;
  const uint32_t list = manager_ ? LoadGuestU32(base_, manager_ + 16) : 0;
  const uint32_t count = manager_ ? std::min(LoadGuestU32(base_, manager_ + 20), 64u) : 0;
  for (uint32_t i = 0; list && i < count; ++i) {
    const uint32_t font = LoadGuestU32(base_, list + i * 4);
    if (Usable(base_, font))
      fonts.push_back(font);
  }
  return fonts;
}

void Canvas::UseFont(uint32_t font) {
  if (!valid_ || !font || font == font_ || !Usable(base_, font))
    return;
  Flush();
  const uint32_t before = raster_;
  Prepare(font);
  if (raster_ != before)
    Bind(raster_);
}

uint32_t Canvas::State(uint32_t state) {
  const uint32_t device = LoadGuestU32(base_, kRenderDevice);
  StoreGuestU32(base_, g_scratch, 0);
  CallGuest(ctx_, base_, LoadGuestU32(base_, device + 36), {state, g_scratch});
  return LoadGuestU32(base_, g_scratch);
}

void Canvas::SetState(uint32_t state, uint32_t value) {
  const uint32_t device = LoadGuestU32(base_, kRenderDevice);
  CallGuest(ctx_, base_, LoadGuestU32(base_, device + 32), {state, value});
}

void Canvas::Bind(uint32_t raster) {
  SetState(kTextureRaster, raster);
}

void Canvas::Flush() {
  if (manager_ && U16(base_, manager_ + 72))
    CallGuest(ctx_, base_, kFlush, {manager_});
}

WidgetScope::WidgetScope(const std::array<float, 6>& transform, const std::array<float, 4>& clip,
                         float alpha)
    : previous_transform_(g_widget.transform),
      previous_clip_(g_widget.clip),
      previous_alpha_(g_widget.alpha),
      previous_enabled_(g_widget.enabled) {
  g_widget = {transform, clip, std::clamp(alpha, 0.f, 1.f), true};
}
WidgetScope::~WidgetScope() {
  g_widget = {previous_transform_, previous_clip_, previous_alpha_, previous_enabled_};
}

void Canvas::Quad(const Vertex (&corners)[4]) {
  const auto append = [&](const Vertex* vertices, size_t size) {
    uint16_t count = U16(base_, manager_ + 72);
    const uint16_t capacity = U16(base_, manager_ + 74);
    if (count + size > capacity) {
      Flush();
      count = U16(base_, manager_ + 72);
      if (count + size > capacity)
        return;
    }
    const uint32_t target = LoadGuestU32(base_, manager_ + 68);
    for (size_t i = 0; i < size; ++i) {
      const auto& v = vertices[i];
      const uint32_t address = target + uint32_t(count + i) * 28;
      StoreGuestFloat(base_, address, v.x);
      StoreGuestFloat(base_, address + 4, v.y);
      StoreGuestFloat(base_, address + 8, z_);
      StoreGuestFloat(base_, address + 12, depth_);
      StoreGuestU32(base_, address + 16, v.color);
      StoreGuestFloat(base_, address + 20, v.u);
      StoreGuestFloat(base_, address + 24, v.v);
    }
    Put16(base_, manager_ + 72, uint16_t(count + size));
  };
  if (!g_widget.enabled) {
    const Vertex vertices[]{corners[0], corners[1], corners[2], corners[3], corners[2], corners[1]};
    append(vertices, 6);
    return;
  }
  // Clip a perimeter-ordered convex quad in the widget's local coordinates.
  std::array<Vertex, 12> polygon{corners[0], corners[1], corners[3], corners[2]}, next{};
  size_t count = 4;
  const auto interpolate = [](const Vertex& a, const Vertex& b, float t) {
    Vertex v{std::lerp(a.x, b.x, t), std::lerp(a.y, b.y, t), std::lerp(a.u, b.u, t),
             std::lerp(a.v, b.v, t), 0};
    for (unsigned shift : {0u, 8u, 16u, 24u}) {
      const float channel =
          std::lerp(float((a.color >> shift) & 255), float((b.color >> shift) & 255), t);
      v.color |= uint32_t(std::clamp(std::lround(channel), 0l, 255l)) << shift;
    }
    return v;
  };
  for (int edge = 0; edge < 4 && count; ++edge) {
    const bool horizontal = edge == 0 || edge == 2;
    const float boundary = g_widget.clip[size_t(edge)];
    const auto distance = [&](const Vertex& v) {
      const float value = horizontal ? v.x : v.y;
      return edge < 2 ? value - boundary : boundary - value;
    };
    size_t written = 0;
    Vertex previous = polygon[count - 1];
    float before = distance(previous);
    for (size_t i = 0; i < count; ++i) {
      const Vertex current = polygon[i];
      const float after = distance(current);
      if ((before >= 0) != (after >= 0))
        next[written++] = interpolate(previous, current, before / (before - after));
      if (after >= 0)
        next[written++] = current;
      previous = current;
      before = after;
    }
    polygon = next;
    count = written;
  }
  if (count < 3)
    return;
  const auto& m = g_widget.transform;
  for (size_t i = 0; i < count; ++i) {
    auto& v = polygon[i];
    const float x = v.x, y = v.y;
    v.x = std::fma(m[0], x, std::fma(m[2], y, m[4]));
    v.y = std::fma(m[1], x, std::fma(m[3], y, m[5]));
    v.color = (v.color & 0xFFFFFF) | (uint32_t(float(v.color >> 24) * g_widget.alpha) << 24);
  }
  std::array<Vertex, 30> triangles{};
  size_t vertices = 0;
  for (size_t i = 1; i + 1 < count; ++i) {
    triangles[vertices++] = polygon[0];
    triangles[vertices++] = polygon[i];
    triangles[vertices++] = polygon[i + 1];
  }
  append(triangles.data(), vertices);
}

void Canvas::Rect(float x, float y, float w, float h, uint32_t color) {
  Gradient(x, y, w, h, color, color);
}

void Canvas::Gradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom) {
  if (!valid_ || w <= 0 || h <= 0)
    return;
  Quad({{x, y, white_u_, white_v_, top},
        {x, y + h, white_u_, white_v_, bottom},
        {x + w, y, white_u_, white_v_, top},
        {x + w, y + h, white_u_, white_v_, bottom}});
}

void Canvas::Frame(float x, float y, float w, float h, float thickness, uint32_t color) {
  Rect(x, y, w, thickness, color);
  Rect(x, y + h - thickness, w, thickness, color);
  Rect(x, y + thickness, thickness, h - thickness * 2, color);
  Rect(x + w - thickness, y + thickness, thickness, h - thickness * 2, color);
}

void Canvas::Circle(float cx, float cy, float radius, uint32_t color) {
  if (!valid_ || radius <= 0)
    return;
  const int strips = std::clamp(int(radius), 8, 48);
  const float step = radius * 2 / strips;
  for (int i = 0; i < strips; ++i) {
    const float y0 = -radius + i * step, y1 = y0 + step;
    const float mid = (y0 + y1) * 0.5f;
    const float half = std::sqrt(std::max(0.0f, radius * radius - mid * mid));
    Rect(cx - half, cy + y0, half * 2, step + 0.25f, color);
  }
}

void Canvas::Triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color) {
  if (!valid_)
    return;
  // A quad with two corners at the same place, wound like Rect's.
  if ((x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0) > 0) {
    std::swap(x1, x2);
    std::swap(y1, y2);
  }
  Quad({{x0, y0, white_u_, white_v_, color},
        {x1, y1, white_u_, white_v_, color},
        {x2, y2, white_u_, white_v_, color},
        {x2, y2, white_u_, white_v_, color}});
}

void Canvas::Line(float x0, float y0, float x1, float y1, float thickness, uint32_t color) {
  if (!valid_)
    return;
  const float dx = x1 - x0, dy = y1 - y0, length = std::hypot(dx, dy);
  if (length <= 0)
    return;
  float nx = -dy / length * thickness * 0.5f, ny = dx / length * thickness * 0.5f;
  // The same winding as Rect's, or the triangles are culled.
  if (dx * ny - dy * nx > 0) {
    nx = -nx;
    ny = -ny;
  }
  Quad({{x0 + nx, y0 + ny, white_u_, white_v_, color},
        {x0 - nx, y0 - ny, white_u_, white_v_, color},
        {x1 + nx, y1 + ny, white_u_, white_v_, color},
        {x1 - nx, y1 - ny, white_u_, white_v_, color}});
}

void Canvas::RoundRect(float x, float y, float w, float h, float radius, uint32_t color) {
  radius = std::min({radius, w * 0.5f, h * 0.5f});
  if (radius < 1) {
    Rect(x, y, w, h, color);
    return;
  }
  // Strips across the rounded top and bottom, a plain middle.
  const int strips = std::clamp(int(radius / 1.5f), 3, 16);
  const float step = radius / strips;
  for (int i = 0; i < strips; ++i) {
    const float dy = radius - (i + 0.5f) * step;
    const float inset = radius - std::sqrt(std::max(0.0f, radius * radius - dy * dy));
    Rect(x + inset, y + i * step, w - inset * 2, step + 0.25f, color);
    Rect(x + inset, y + h - (i + 1) * step - 0.25f, w - inset * 2, step + 0.25f, color);
  }
  Rect(x, y + radius, w, h - radius * 2, color);
}

void Canvas::Shape(const float (&x)[4], const float (&y)[4], uint32_t color) {
  if (!valid_)
    return;
  // Wound like Rect's, or the triangles are culled.
  const bool flip = (x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0]) > 0;
  const int a = flip ? 2 : 1, b = flip ? 1 : 2;
  Quad({{x[0], y[0], white_u_, white_v_, color},
        {x[a], y[a], white_u_, white_v_, color},
        {x[b], y[b], white_u_, white_v_, color},
        {x[3], y[3], white_u_, white_v_, color}});
}

bool Canvas::Image(const Picture& picture, float x, float y, float w, float h, uint32_t color,
                   float angle) {
  if (!valid_ || !g_pictures || g_pictures_failed || !picture.id || !picture.width ||
      !picture.height || picture.rgba.size() < size_t(picture.width) * picture.height * 4 ||
      w <= 0 || h <= 0)
    return false;
  if (!g_pictures_built) {
    g_pictures_failed = !BuildPictures(base_, raster_);
    g_pictures_built = !g_pictures_failed;
    if (g_pictures_failed)
      return false;
  }
  uint32_t slot = kSlots;
  for (uint32_t i = 0; i < kSlots; ++i)
    if (g_slots[i].id == picture.id)
      slot = i;
  if (slot == kSlots) {
    // The slot drawn longest ago, never one drawn by this canvas.
    slot = 0;
    for (uint32_t i = 1; i < kSlots; ++i)
      if (g_slots[i].drawn < g_slots[slot].drawn)
        slot = i;
    if (g_slots[slot].id && g_slots[slot].drawn == g_canvases)
      return false;
    Upload(base_, slot, picture);
  }
  Slot& s = g_slots[slot];
  s.drawn = g_canvases;
  Flush();
  Bind(SlotRaster(slot));
  // Half a texel in from the edges, so filtering never reaches the padding.
  const float u0 = 0.5f / kSlotWidth, v0 = 0.5f / kSlotHeight;
  const float u1 = (s.width - 0.5f) / kSlotWidth, v1 = (s.height - 0.5f) / kSlotHeight;
  // The corners turned about the middle.
  const float cx = x + w * 0.5f, cy = y + h * 0.5f, cs = std::cos(angle), sn = std::sin(angle);
  const auto at = [&](float px, float py, float u, float v) {
    const float dx = px - cx, dy = py - cy;
    return Vertex{cx + dx * cs - dy * sn, cy + dx * sn + dy * cs, u, v, color};
  };
  Quad({at(x, y, u0, v0), at(x, y + h, u0, v1), at(x + w, y, u1, v0), at(x + w, y + h, u1, v1)});
  Flush();
  Bind(raster_);
  return true;
}

std::vector<std::string> Canvas::Wrap(float size, std::string_view text, float max_width) const {
  std::vector<std::string> lines;
  std::string line;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find_first_of(" \n", start);
    if (end == std::string_view::npos)
      end = text.size();
    const std::string word(text.substr(start, end - start));
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (!line.empty() && Measure(size, candidate) > max_width) {
      lines.push_back(line);
      line = word;
    } else {
      line = candidate;
    }
    if (end < text.size() && text[end] == '\n') {
      lines.push_back(line);
      line.clear();
    }
    start = end + 1;
  }
  if (!line.empty())
    lines.push_back(line);
  return lines;
}

float Canvas::Measure(float size, std::string_view utf8) const {
  return MeasureLatin1(size, Latin1(utf8));
}

float Canvas::MeasureLatin1(float size, std::string_view text) const {
  if (!font_)
    return 0;
  const float line = float(LoadGuestU32(base_, font_ + 24));
  if (line <= 0)
    return 0;
  float advance = 0;
  for (const unsigned char c : text) {
    const uint32_t glyph = Glyph(base_, font_, c);
    if (glyph)
      advance += U16(base_, glyph + 6);
  }
  return advance * size / line;
}

std::string_view Canvas::Fit(float size, std::string_view text, float max_width) const {
  if (Measure(size, text) <= max_width)
    return text;
  static thread_local std::string fitted;
  fitted.assign(text);
  while (!fitted.empty() && Measure(size, fitted + "...") > max_width)
    fitted.pop_back();
  fitted += "...";
  return fitted;
}

float Canvas::Text(float x, float y, float size, uint32_t color, std::string_view utf8,
                   Align align) {
  if (!valid_ || utf8.empty())
    return 0;
  const std::string text = Latin1(utf8);
  const float line = float(LoadGuestU32(base_, font_ + 24));
  if (line <= 0)
    return 0;
  const float scale = size / line, width = MeasureLatin1(size, text);
  if (align == Align::kCenter)
    x -= width * 0.5f;
  else if (align == Align::kRight)
    x -= width;
  for (const unsigned char c : text) {
    const uint32_t glyph = Glyph(base_, font_, c);
    if (!glyph)
      continue;
    const float left = x + int16_t(U16(base_, glyph + 8)) * scale;
    const float top = y + int16_t(U16(base_, glyph + 10)) * scale;
    const float w = U16(base_, glyph + 2) * scale, h = U16(base_, glyph + 4) * scale;
    const float u0 = U16(base_, glyph + 12) / (16.0f * atlas_width_);
    const float v0 = U16(base_, glyph + 14) / (16.0f * atlas_height_);
    const float u1 = U16(base_, glyph + 16) / (16.0f * atlas_width_);
    const float v1 = U16(base_, glyph + 18) / (16.0f * atlas_height_);
    if (w > 0 && h > 0)
      Quad({{left, top, u0, v0, color},
            {left, top + h, u0, v1, color},
            {left + w, top, u1, v0, color},
            {left + w, top + h, u1, v1, color}});
    x += U16(base_, glyph + 6) * scale;
  }
  return width;
}

}  // namespace canvas
