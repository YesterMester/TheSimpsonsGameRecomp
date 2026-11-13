#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct PPCContext;

// Drawing with the game's own 2D renderer, in its own fonts: the batch of
// textured quads its font manager draws text with (game_prompts.cpp draws
// button prompts the same way). The debug menu and the achievements screen are
// drawn with it, inside the game's frame.
namespace canvas {

enum class Align { kLeft, kCenter, kRight };

// A picture the canvas can draw: 8-bit RGBA, top row first. id tells
// pictures apart (a different id for different pixels; 0 is none).
struct Picture {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
  uint64_t id = 0;
};

// Guest memory the GPU reads pictures from: physical, page aligned.
constexpr uint32_t kPictureMemory = 4096 + 3 * 512 * 288 * 4;

// Guest memory for the quads and for reading render states, and for
// pictures (kPictureMemory bytes; 0 for none).
void Init(uint32_t scratch, uint32_t vertices, uint32_t vertex_capacity, uint32_t pictures);

// A widget in an APT movie uses its parent's transform and local clipping
// rectangle. Geometry is clipped before transforming, including texture UVs.
// This scope does not change Canvas's object layout or the default HUD path.
class WidgetScope {
 public:
  WidgetScope(const std::array<float, 6>& transform, const std::array<float, 4>& clip,
              float alpha = 1);
  ~WidgetScope();
  WidgetScope(const WidgetScope&) = delete;
  WidgetScope& operator=(const WidgetScope&) = delete;

 private:
  std::array<float, 6> previous_transform_;
  std::array<float, 4> previous_clip_;
  float previous_alpha_;
  bool previous_enabled_;
};

class Canvas {
 public:
  // Begins drawing over the current 2D target; valid() says whether it can.
  Canvas(PPCContext& ctx, uint8_t* base);
  ~Canvas();
  Canvas(const Canvas&) = delete;
  Canvas& operator=(const Canvas&) = delete;

  bool valid() const { return valid_; }
  // The target's size in pixels.
  float width() const { return width_; }
  float height() const { return height_; }

  // Colours are 0xAARRGGBB.
  void Rect(float x, float y, float w, float h, uint32_t color);
  void Gradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom);
  void Frame(float x, float y, float w, float h, float thickness, uint32_t color);
  void Circle(float cx, float cy, float radius, uint32_t color);
  void Triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color);
  void Line(float x0, float y0, float x1, float y1, float thickness, uint32_t color);
  // A rectangle with rounded corners.
  void RoundRect(float x, float y, float w, float h, float radius, uint32_t color);
  // A solid four-cornered shape: top-left, bottom-left, top-right,
  // bottom-right.
  void Shape(const float (&x)[4], const float (&y)[4], uint32_t color);
  // A picture scaled into a rectangle, turned by angle (radians, clockwise)
  // about its middle and tinted by color (white leaves it as it is). Pictures
  // larger than 512x288 are cut down to it. False if it can't be drawn.
  bool Image(const Picture& picture, float x, float y, float w, float h,
             uint32_t color = 0xFFFFFFFF, float angle = 0);

  // Text with its top-left (or centre or right) at x, y; size is the line
  // height in pixels. Returns the width drawn.
  float Text(float x, float y, float size, uint32_t color, std::string_view text,
             Align align = Align::kLeft);
  float Measure(float size, std::string_view text) const;
  // Shortens text with "..." to fit a width.
  std::string_view Fit(float size, std::string_view text, float max_width) const;
  // Splits text into lines no wider than max_width.
  std::vector<std::string> Wrap(float size, std::string_view text, float max_width) const;
  // The font text is drawn in now (UseFont, or the game's current one).
  uint32_t font() const { return font_; }

  // The game's loaded fonts, and the one text uses (0 = the default).
  std::vector<uint32_t> Fonts() const;
  void UseFont(uint32_t font);

 private:
  struct Vertex {
    float x, y, u, v;
    uint32_t color;
  };
  void Quad(const Vertex (&corners)[4]);
  float MeasureLatin1(float size, std::string_view text) const;
  void Flush();
  void Bind(uint32_t raster);
  uint32_t State(uint32_t state);
  void SetState(uint32_t state, uint32_t value);
  void Prepare(uint32_t font);

  PPCContext& ctx_;
  uint8_t* base_;
  uint32_t manager_ = 0, camera_ = 0, font_ = 0, raster_ = 0;
  float width_ = 0, height_ = 0;
  float atlas_width_ = 0, atlas_height_ = 0, white_u_ = 0, white_v_ = 0;
  float z_ = 0, depth_ = 0;
  uint32_t saved_raster_ = 0, saved_states_[6] = {};
  uint32_t saved_buffer_ = 0, saved_count_ = 0, saved_capacity_ = 0;
  bool valid_ = false;
};

}  // namespace canvas
