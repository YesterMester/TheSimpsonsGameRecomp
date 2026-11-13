// The achievements screen, drawn in the game's own frame (game_ui.cpp): every
// achievement with its picture, gamerscore and date, filtered by All, Unlocked
// and Locked. The pictures are the title data's, drawn as small squares of
// colour: the game's 2D renderer has no texture of them.

#include "achievements.h"
#include "debug_menu.h"
#include "game_canvas.h"
#include "game_ui.h"
#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/platform.h>

namespace game_ui {
namespace {

using achievements::Achievement;

enum class Filter { kAll, kUnlocked, kLocked };

// A picture as runs of one colour along each row of a small grid.
struct Run {
  uint8_t x0, x1, y;
  uint32_t color;
};
struct Picture {
  std::vector<Run> color, faded;
  int size = 0;
  bool tried = false;
};
constexpr int kPictureSize = 32;

struct Cache {
  uint32_t generation = ~0u;
  std::vector<Achievement> list;
  uint32_t unlocked = 0, score = 0, total_score = 0;
  bool with_cheats = false;
};
Cache g_cache;
std::map<uint32_t, Picture> g_pictures;
Filter g_filter = Filter::kAll;
int g_selected = 0;
float g_top = 0;  // the first row shown, in rows

void Refresh() {
  const uint32_t generation = achievements::Generation();
  if (generation == g_cache.generation)
    return;
  g_cache.generation = generation;
  g_cache.list = achievements::List();
  g_cache.unlocked = g_cache.score = g_cache.total_score = 0;
  g_cache.with_cheats = false;
  for (const auto& achievement : g_cache.list) {
    g_cache.total_score += achievement.gamerscore;
    if (achievement.unlocked) {
      ++g_cache.unlocked;
      g_cache.score += achievement.gamerscore;
      g_cache.with_cheats |= achievement.with_cheats;
    }
  }
}

std::vector<Run> Runs(const std::vector<uint8_t>& rgba, int width, int height, bool faded) {
  // Averaged down to the grid, colours rounded so neighbours join up.
  std::vector<Run> runs;
  const auto pixel = [&](int gx, int gy) {
    float sum[4] = {};
    int n = 0;
    for (int y = gy * height / kPictureSize; y < (gy + 1) * height / kPictureSize; ++y)
      for (int x = gx * width / kPictureSize; x < (gx + 1) * width / kPictureSize; ++x) {
        const uint8_t* p = &rgba[(size_t(y) * width + x) * 4];
        for (int c = 0; c < 4; ++c)
          sum[c] += p[c];
        ++n;
      }
    if (!n)
      return 0u;
    float r = sum[0] / n, g = sum[1] / n, b = sum[2] / n;
    const float a = sum[3] / n;
    if (faded) {
      // Locked: greyscale, darker, with a little of the night-sky blue.
      const float luma = (0.299f * r + 0.587f * g + 0.114f * b) * 0.45f;
      r = luma * 0.9f;
      g = luma * 0.97f;
      b = std::min(255.0f, luma * 1.15f + 8);
    }
    const auto q = [](float v) { return uint32_t(std::clamp(int(v + 4) & ~7, 0, 255)); };
    return (uint32_t(std::clamp(a, 0.0f, 255.0f)) << 24) | q(r) << 16 | q(g) << 8 | q(b);
  };
  for (int y = 0; y < kPictureSize; ++y) {
    int start = 0;
    uint32_t current = pixel(0, y);
    for (int x = 1; x <= kPictureSize; ++x) {
      const uint32_t next = x < kPictureSize ? pixel(x, y) : ~current;
      if (next != current) {
        if (current >> 24)
          runs.push_back({uint8_t(start), uint8_t(x), uint8_t(y), current});
        start = x;
        current = next;
      }
    }
  }
  return runs;
}

const Picture& GetPicture(const Achievement& achievement) {
  Picture& picture = g_pictures[achievement.id];
  if (!picture.tried) {
    picture.tried = true;
    int width = 0, height = 0;
    const auto rgba = overlay::DecodePicture(achievement.icon_png, &width, &height);
    if (!rgba.empty() && width >= kPictureSize && height >= kPictureSize) {
      picture.color = Runs(rgba, width, height, false);
      picture.faded = Runs(rgba, width, height, true);
      picture.size = kPictureSize;
    }
  }
  return picture;
}

void DrawPicture(Frame& f, const Achievement& achievement, float x, float y, float size) {
  const Picture& picture = GetPicture(achievement);
  if (!picture.size) {
    f.c.RoundRect(x, y, size, size, 8 * f.s, 0xFF183463);
    return;
  }
  const float cell = size / float(picture.size);
  for (const Run& run : achievement.unlocked ? picture.color : picture.faded)
    f.c.Rect(x + run.x0 * cell, y + run.y * cell, (run.x1 - run.x0) * cell + 0.6f, cell + 0.6f,
             run.color);
}

std::string FormatDate(int64_t unix_time) {
  if (unix_time <= 0)
    return "Unlocked";
  const std::time_t time = std::time_t(unix_time);
  std::tm local{};
#if REX_PLATFORM_WIN32
  localtime_s(&local, &time);
#else
  localtime_r(&time, &local);
#endif
  char text[64];
  std::strftime(text, sizeof(text), "Unlocked %d %b %Y", &local);
  return text;
}

std::vector<int> Visible() {
  std::vector<int> indices;
  for (int i = 0; i < int(g_cache.list.size()); ++i) {
    const bool unlocked = g_cache.list[size_t(i)].unlocked;
    if (g_filter == Filter::kAll || (g_filter == Filter::kUnlocked) == unlocked)
      indices.push_back(i);
  }
  return indices;
}

// Yellow letters with a dark outline, like the show's title card.
void TitleText(Frame& f, float x, float y, float size, std::string_view text,
               uint32_t fill = 0xFFFFD90F, canvas::Align align = canvas::Align::kLeft) {
  auto& c = f.c;
  c.UseFont(f.title_font);
  const float o = std::max(1.0f, size * 0.055f);
  for (int dx = -1; dx <= 1; ++dx)
    for (int dy = -1; dy <= 1; ++dy)
      if (dx || dy)
        c.Text(x + dx * o, y + dy * o, size, 0xFF281600, text, align);
  c.Text(x + o * 0.6f, y + o * 1.6f, size, 0xFF281600, text, align);
  c.Text(x, y, size, fill, text, align);
}

void Cloud(Frame& f, float cx, float cy, float size) {
  const struct {
    float x, y, r;
  } puffs[] = {{-0.55f, 0.12f, 0.36f}, {-0.2f, -0.12f, 0.46f}, {0.25f, -0.05f, 0.42f},
               {0.6f, 0.15f, 0.32f},   {0.0f, 0.2f, 0.4f}};
  for (const auto& puff : puffs)
    f.c.Circle(cx + puff.x * size, cy + puff.y * size + size * 0.05f, puff.r * size, 0xEBC8E4FA);
  for (const auto& puff : puffs)
    f.c.Circle(cx + puff.x * size, cy + puff.y * size, puff.r * size, 0xEBFFFFFF);
}

void ProgressBar(Frame& f, float x, float y, float w, float h, float fraction) {
  auto& c = f.c;
  c.RoundRect(x, y, w, h, h * 0.5f, 0xFF050E20);
  fraction = std::clamp(fraction, 0.0f, 1.0f);
  if (fraction > 0) {
    const float fill = std::max(w * fraction, h);
    c.RoundRect(x, y, fill, h, h * 0.5f, 0xFFFFD90F);
    c.RoundRect(x + h * 0.3f, y + h * 0.18f, std::max(0.0f, fill - h * 0.6f), h * 0.22f, h * 0.11f,
                0x96FFF5BE);
  }
}

void Card(Frame& f, const Achievement& achievement, float x, float y, float w, float h,
          bool selected) {
  auto& c = f.c;
  const float s = f.s, r = 16 * s;
  const bool unlocked = achievement.unlocked;
  if (selected)
    c.RoundRect(x - 5 * s, y - 5 * s, w + 10 * s, h + 10 * s, r + 5 * s, 0xFFFFD90F);
  c.RoundRect(x, y, w, h, r, unlocked ? 0xFF1A3C78 : 0xFF13213E);
  c.RoundRect(x, y, w, h * 0.5f, r, unlocked ? 0xFF22487F : 0xFF172747);
  c.Rect(x, y + h * 0.5f - r, w, r, unlocked ? 0xFF22487F : 0xFF172747);

  // The picture in a frame.
  const float pad = 12 * s, icon = h - pad * 2;
  c.RoundRect(x + pad - 3 * s, y + pad - 3 * s, icon + 6 * s, icon + 6 * s, 10 * s,
              unlocked ? 0xFFFFD90F : 0xFF3C4C68);
  DrawPicture(f, achievement, x + pad, y + pad, icon);
  if (!unlocked) {
    const float lx = x + pad + icon - 14 * s, ly = y + pad + icon - 14 * s;
    c.Circle(lx, ly, 16 * s, 0xEB081228);
    Padlock(f, lx, ly - 2 * s, 17 * s, 0xFFFFD90F);
  }

  // Gamerscore.
  const std::string points = std::to_string(achievement.gamerscore) + "G";
  c.UseFont(f.title_font);
  const float points_size = 24 * s, points_w = c.Measure(points_size, points) + 22 * s;
  const float bx = x + w - pad - points_w, by = y + pad, bh = points_size + 8 * s;
  c.RoundRect(bx, by, points_w, bh, bh * 0.5f, unlocked ? 0xFFFFD90F : 0xFF283A5C);
  c.Text(bx + points_w * 0.5f, by + 4 * s, points_size, unlocked ? 0xFF281600 : 0xFF96A8C4, points,
         canvas::Align::kCenter);

  // Name and description.
  const float tx = x + pad + icon + 16 * s, tw = bx - 10 * s - tx;
  c.UseFont(f.title_font);
  const float name_size = 25 * s;
  c.Text(tx, y + pad - 1 * s, name_size, unlocked ? 0xFFF5F7FA : 0xFFC8D2E4,
         c.Fit(name_size, achievement.name, tw));
  const std::string& description = unlocked || achievement.locked_description.empty()
                                       ? achievement.description
                                       : achievement.locked_description;
  c.UseFont(f.body_font);
  const float body = 19 * s;
  const auto lines = c.Wrap(body, description, w - (tx - x) - pad);
  float ly = y + pad + name_size + 4 * s;
  for (size_t i = 0; i < lines.size() && i < 2; ++i) {
    c.Text(tx, ly, body, 0xFF96A8C4, i == 1 && lines.size() > 2 ? c.Fit(body, lines[i] + " ...", w)
                                                                : std::string_view(lines[i]));
    ly += body * 1.15f;
  }

  // The date, or how far along the loaded save is.
  const float foot = y + h - pad - 19 * s;
  if (unlocked) {
    const float cx = tx + 8 * s, cy = foot + 9 * s;
    c.Circle(cx, cy, 9 * s, 0xFF6ED65C);
    c.Line(cx - 4.5f * s, cy, cx - 1 * s, cy + 3.5f * s, 2.4f * s, 0xFFFFFFFF);
    c.Line(cx - 1 * s, cy + 3.5f * s, cx + 5 * s, cy - 3.5f * s, 2.4f * s, 0xFFFFFFFF);
    const std::string date = FormatDate(achievement.unlock_time);
    c.UseFont(f.body_font);
    const float dw = c.Text(tx + 24 * s, foot, 18 * s, 0xFF6ED65C, date);
    if (achievement.with_cheats) {
      c.UseFont(f.title_font);
      const float tag_x = tx + 34 * s + dw, tag_w = c.Measure(14 * s, "CHEATS") + 14 * s;
      c.RoundRect(tag_x, foot, tag_w, 20 * s, 6 * s, 0xFFF05A9D);
      c.Text(tag_x + tag_w * 0.5f, foot + 3 * s, 14 * s, 0xFFFFFFFF, "CHEATS",
             canvas::Align::kCenter);
    }
  } else if (achievement.goal > 1) {
    const std::string count =
        fmt::format("{} / {}", int(achievement.progress), int(achievement.goal));
    c.UseFont(f.title_font);
    const float cw = c.Measure(17 * s, count);
    ProgressBar(f, tx, foot + 4 * s, w - (tx - x) - pad - cw - 12 * s, 12 * s,
                achievement.progress / achievement.goal);
    c.Text(x + w - pad, foot, 17 * s, 0xFF96A8C4, count, canvas::Align::kRight);
  } else {
    c.UseFont(f.body_font);
    c.Text(tx, foot, 18 * s, 0xFF7890AA, "Locked");
  }
}

}  // namespace

bool DrawAchievements(Frame& f, bool appearing) {
  Refresh();
  auto& c = f.c;
  const float s = f.s;
  const Input& in = f.input;
  if (appearing) {
    g_selected = 0;
    g_top = 0;
    REXLOG_INFO("[achievements] screen opened");
  }
  if (in.back)
    return false;
  if (in.previous || in.next) {
    g_filter = Filter((int(g_filter) + (in.next ? 1 : 2)) % 3);
    g_selected = 0;
    g_top = 0;
    Sound("ui_move_up_down");
  }

  const float panel_w = std::min(c.width() - 40 * s, 1130 * s);
  const float panel_h = std::min(c.height() - 40 * s, 650 * s);
  const float px = (c.width() - panel_w) * 0.5f, py = (c.height() - panel_h) * 0.5f;
  // Dim the game, then the panel with a yellow edge.
  c.Rect(0, 0, c.width(), c.height(), 0x99000000);
  c.RoundRect(px - 4 * s, py - 4 * s, panel_w + 8 * s, panel_h + 8 * s, 30 * s, 0xFFFFD90F);
  c.RoundRect(px, py, panel_w, panel_h, 26 * s, 0xF50B1B3A);

  // The sky header.
  const float header = 112 * s;
  c.RoundRect(px, py, panel_w, header, 26 * s, 0xFF4BB4EC);
  c.Gradient(px, py + 26 * s, panel_w, header - 26 * s, 0xFF4BB4EC, 0xFF2E86C8);
  Cloud(f, px + panel_w * 0.47f, py + 34 * s, 34 * s);
  Cloud(f, px + panel_w * 0.72f, py + 82 * s, 28 * s);
  Cloud(f, px + panel_w * 0.6f, py + 22 * s, 20 * s);
  TitleText(f, px + 30 * s, py + 22 * s, 60 * s, "ACHIEVEMENTS");
  const std::string score = fmt::format("{} / {}G", g_cache.score, g_cache.total_score);
  TitleText(f, px + panel_w - 30 * s, py + 14 * s, 46 * s, score, 0xFFFFD90F,
            canvas::Align::kRight);
  c.UseFont(f.title_font);
  c.Text(px + panel_w - 30 * s, py + 66 * s, 22 * s, 0xFFFFFFFF,
         fmt::format("{} of {} unlocked", g_cache.unlocked, g_cache.list.size()),
         canvas::Align::kRight);
  float y = py + header;
  ProgressBar(f, px + 30 * s, y - 16 * s, panel_w - 60 * s, 12 * s,
              g_cache.list.empty() ? 0 : float(g_cache.unlocked) / float(g_cache.list.size()));
  y += 10 * s;

  // Cheats: achievements still unlock; this only says the game was modified.
  if (debug_menu::CheatsEnabled() || g_cache.with_cheats) {
    const float bh = 40 * s;
    c.RoundRect(px + 24 * s, y, panel_w - 48 * s, bh, 12 * s, 0xFFF05A9D);
    c.RoundRect(px + 27 * s, y + 3 * s, panel_w - 54 * s, bh - 6 * s, 10 * s, 0xFF6E1E46);
    TitleText(f, px + 44 * s, y + (bh - 24 * s) * 0.5f, 24 * s, "CHEATS DETECTED", 0xFFFFFFFF);
    c.UseFont(f.body_font);
    c.Text(px + 44 * s + c.Measure(24 * s, "CHEATS DETECTED") * 1.25f + 20 * s,
           y + (bh - 21 * s) * 0.5f, 21 * s, 0xFFFFE4F0,
           debug_menu::CheatsEnabled()
               ? "Cheats are on. Achievements still unlock: this only shows the game is modified."
               : "Some were unlocked with cheats on. They still count; the game was modified.");
    y += bh + 10 * s;
  }

  // The filters, with LB and RB.
  {
    const char* names[] = {"All", "Unlocked", "Locked"};
    const uint32_t counts[] = {uint32_t(g_cache.list.size()), g_cache.unlocked,
                               uint32_t(g_cache.list.size()) - g_cache.unlocked};
    float x = px + 30 * s;
    x += Glyph(f, x, y + 4 * s, 32 * s, "LB") + 12 * s;
    for (int i = 0; i < 3; ++i) {
      const std::string label = fmt::format("{}  {}", names[i], counts[i]);
      c.UseFont(f.title_font);
      const float tw = c.Measure(22 * s, label) + 36 * s, th = 40 * s;
      const bool active = int(g_filter) == i;
      c.RoundRect(x, y, tw, th, th * 0.5f, active ? 0xFFFFD90F : 0xFF183463);
      c.Text(x + tw * 0.5f, y + (th - 22 * s) * 0.5f, 22 * s, active ? 0xFF281600 : 0xFFE6ECF5,
             label, canvas::Align::kCenter);
      if (in.tapped && in.tap_x >= x && in.tap_x < x + tw && in.tap_y >= y && in.tap_y < y + th &&
          g_filter != Filter(i)) {
        g_filter = Filter(i);
        g_selected = 0;
        g_top = 0;
        Sound("ui_move_up_down");
      }
      x += tw + 10 * s;
    }
    Glyph(f, x + 2 * s, y + 4 * s, 32 * s, "RB");
    y += 52 * s;
  }

  // The cards, two to a row.
  const auto visible = Visible();
  const int columns = 2, count = int(visible.size());
  const int rows = (count + columns - 1) / columns;
  const float gap = 14 * s, card_h = 104 * s;
  const float list_x = px + 24 * s, list_w = panel_w - 48 * s - 14 * s;
  const float card_w = (list_w - gap) / columns;
  const float footer = 56 * s;
  const float list_top = y, list_h = py + panel_h - footer - y;
  const int shown_rows = std::max(1, int((list_h + gap) / (card_h + gap)));
  if (count) {
    g_selected = std::clamp(g_selected, 0, count - 1);
    int moved = g_selected;
    if (in.up)
      moved -= columns;
    if (in.down)
      moved += columns;
    if (in.left)
      moved -= 1;
    if (in.right)
      moved += 1;
    if (moved != g_selected && moved >= 0 && moved < count) {
      g_selected = moved;
      Sound("ui_move_up_down");
    }
    const int row = g_selected / columns;
    if (in.scroll != 0)
      g_top += in.scroll;
    else if (!in.pointer || in.up || in.down || in.left || in.right) {
      if (row < int(g_top))
        g_top = float(row);
      if (row >= int(g_top) + shown_rows)
        g_top = float(row - shown_rows + 1);
    }
    g_top = std::clamp(g_top, 0.0f, float(std::max(0, rows - shown_rows)));
    const int first = int(g_top);
    for (int r = first; r < std::min(rows, first + shown_rows); ++r) {
      for (int col = 0; col < columns; ++col) {
        const int index = r * columns + col;
        if (index >= count)
          break;
        const float cx = list_x + col * (card_w + gap);
        const float cy = list_top + (r - first) * (card_h + gap);
        if (in.tapped && in.tap_x >= cx && in.tap_x < cx + card_w && in.tap_y >= cy &&
            in.tap_y < cy + card_h)
          g_selected = index;
        Card(f, g_cache.list[size_t(visible[size_t(index)])], cx, cy, card_w, card_h,
             index == g_selected);
      }
    }
    // Where the list is.
    if (rows > shown_rows) {
      const float track_x = px + panel_w - 26 * s, track_h = list_h - gap;
      c.RoundRect(track_x, list_top, 8 * s, track_h, 4 * s, 0x40FFFFFF);
      const float thumb_h = std::max(30 * s, track_h * float(shown_rows) / float(rows));
      const float thumb_y =
          list_top + (track_h - thumb_h) * (g_top / float(std::max(1, rows - shown_rows)));
      c.RoundRect(track_x, thumb_y, 8 * s, thumb_h, 4 * s, 0xD0FFD90F);
    }
  } else {
    c.UseFont(f.title_font);
    c.Text(px + panel_w * 0.5f, list_top + 60 * s, 28 * s, 0xFF96A8C4,
           g_filter == Filter::kUnlocked ? "Nothing unlocked yet" : "Everything is unlocked!",
           canvas::Align::kCenter);
  }

  // How to get around; B is a button for the mouse and touch too.
  {
    const float fy = py + panel_h - footer + 10 * s;
    float x = px + panel_w - 30 * s;
    c.UseFont(f.title_font);
    const float back_w = c.Measure(24 * s, "Back");
    x -= back_w;
    c.Text(x, fy + 5 * s, 24 * s, 0xFFF5F7FA, "Back");
    x -= 44 * s;
    Glyph(f, x, fy, 34 * s, "B");
    if (in.tapped && in.tap_x >= x && in.tap_x < px + panel_w - 20 * s && in.tap_y >= fy &&
        in.tap_y < fy + 34 * s)
      return false;
    x -= 40 * s;
    const float filter_w = c.Measure(24 * s, "Filter");
    x -= filter_w;
    c.Text(x, fy + 5 * s, 24 * s, 0xFFF5F7FA, "Filter");
    x -= 50 * s;
    Glyph(f, x, fy, 34 * s, "RB");
    x -= 50 * s;
    Glyph(f, x, fy, 34 * s, "LB");
  }
  return Current() == Screen::kAchievements;
}

}  // namespace game_ui
