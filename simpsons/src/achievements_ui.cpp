// Achievement unlock notifications, the one part of the UI drawn over the game
// rather than in it (the achievements screen is in the game:
// achievements_screen.cpp).

#include "achievements.h"
#include "overlay.h"
#include "overlay_draw.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <deque>
#include <string>

#include <rex/cvar.h>
#include <rex/platform.h>

REXCVAR_DEFINE_BOOL(achievement_notifications, true, "Game",
                    "Show a notification when an achievement is unlocked");

namespace overlay {
namespace {

using achievements::Achievement;

struct Cache {
  uint32_t generation = ~0u;
  std::vector<Achievement> list;
  uint32_t unlocked = 0, score = 0, total_score = 0;
  bool with_cheats = false;  // any unlocked while cheats were on
};
Cache g_cache;

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

const Achievement* FindCached(uint32_t id) {
  for (const auto& achievement : g_cache.list) {
    if (achievement.id == id)
      return &achievement;
  }
  return nullptr;
}

float EaseOutBack(float t) {
  const float c1 = 1.70158f, c3 = c1 + 1;
  return 1 + c3 * std::pow(t - 1, 3.0f) + c1 * std::pow(t - 1, 2.0f);
}

ImU32 WithAlpha(ImU32 color, float alpha) {
  const uint32_t a = uint32_t(float((color >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

}  // namespace

namespace {

struct Toast {
  std::vector<uint32_t> ids;  // more than one: unlocked together from a save
  float time = 0;
};
std::deque<Toast> g_toasts;
constexpr float kToastTime = 5.5f, kToastIn = 0.45f, kToastOut = 0.45f;

}  // namespace

bool DrawNotifications() {
  const auto ids = achievements::TakeNotifications();
  if (!ids.empty() && REXCVAR_GET(achievement_notifications)) {
    Refresh();
    // A loaded save that earned several at once gets one notification.
    if (ids.size() >= 4)
      g_toasts.push_back({ids, 0});
    else
      for (const uint32_t id : ids)
        g_toasts.push_back({{id}, 0});
  }
  if (g_toasts.empty())
    return false;
  Refresh();
  Toast& toast = g_toasts.front();
  toast.time += FrameTime();
  if (toast.time >= kToastTime) {
    g_toasts.pop_front();
    return !g_toasts.empty();
  }
  const float s = Scale();
  ImDrawList* draw = ImGui::GetForegroundDrawList();

  // The contents.
  std::string headline, line;
  const Achievement* picture = nullptr;
  if (toast.ids.size() == 1) {
    picture = FindCached(toast.ids[0]);
    headline = "ACHIEVEMENT UNLOCKED";
    if (picture)
      line = std::to_string(picture->gamerscore) + "G \xE2\x80\x93 " + picture->name;
  } else {
    uint32_t points = 0;
    for (const uint32_t id : toast.ids) {
      if (const auto* achievement = FindCached(id)) {
        points += achievement->gamerscore;
        if (!picture || achievement->gamerscore > picture->gamerscore)
          picture = achievement;
      }
    }
    headline = std::to_string(toast.ids.size()) + " ACHIEVEMENTS UNLOCKED";
    line = std::to_string(points) + "G earned by your saved game";
  }

  // In, hold, out.
  float appear = 1, alpha = 1;
  if (toast.time < kToastIn)
    appear = EaseOutBack(toast.time / kToastIn);
  else if (toast.time > kToastTime - kToastOut)
    alpha = (kToastTime - toast.time) / kToastOut;
  const float height = 92 * s;
  const float head_size = 20 * s, line_size = 27 * s;
  const float text_w = std::max(MeasureText(DisplayFont(), head_size, headline).x,
                                MeasureText(BoldFont(), line_size, line).x);
  const float width = std::max(460 * s, height + 26 * s + text_w + 40 * s);
  const ImVec2 game_min = GameMin(), game_max = GameMax();
  const float y = game_max.y - 48 * s - height * appear;
  const ImVec2 min((game_min.x + game_max.x - width) * 0.5f, y);
  const ImVec2 max(min.x + width, y + height);
  const float rounding = height * 0.5f;
  draw->AddRectFilled(ImVec2(min.x + 4 * s, min.y + 6 * s), ImVec2(max.x + 4 * s, max.y + 6 * s),
                      WithAlpha(IM_COL32(0, 0, 0, 120), alpha), rounding);
  draw->AddRectFilled(min, max, WithAlpha(IM_COL32(10, 24, 52, 245), alpha), rounding);
  // A shine passing over after it lands.
  const float shine = (toast.time - kToastIn) / 0.8f;
  if (shine > 0 && shine < 1) {
    draw->PushClipRect(min, max, true);
    const float sx = min.x + (width + height) * shine - height;
    draw->AddQuadFilled(ImVec2(sx, max.y), ImVec2(sx + height * 0.5f, min.y),
                        ImVec2(sx + height * 0.9f, min.y), ImVec2(sx + height * 0.4f, max.y),
                        WithAlpha(IM_COL32(255, 255, 255, 40), alpha));
    draw->PopClipRect();
  }
  draw->AddRect(min, max, WithAlpha(color::kYellow, alpha), rounding, 0, 3 * s);

  // Picture in a yellow ring.
  const float icon = height - 20 * s;
  const ImVec2 center(min.x + 10 * s + icon * 0.5f, min.y + height * 0.5f);
  draw->AddCircleFilled(center, icon * 0.5f + 4 * s, WithAlpha(color::kYellow, alpha), 48);
  const ImTextureID texture = picture ? Picture(picture->id, picture->icon_png) : ImTextureID{};
  if (texture) {
    draw->AddImageRounded(ImTextureRef(texture), ImVec2(center.x - icon * 0.5f, center.y - icon * 0.5f),
                          ImVec2(center.x + icon * 0.5f, center.y + icon * 0.5f), ImVec2(0, 0),
                          ImVec2(1, 1), WithAlpha(IM_COL32_WHITE, alpha), icon * 0.5f);
  }
  const float text_x = center.x + icon * 0.5f + 20 * s;
  Text(draw, DisplayFont(), head_size, ImVec2(text_x, min.y + 15 * s),
       WithAlpha(color::kYellow, alpha), headline);
  Text(draw, BoldFont(), line_size, ImVec2(text_x, min.y + 15 * s + head_size + 8 * s),
       WithAlpha(color::kText, alpha), line);
  return true;
}

}  // namespace overlay
